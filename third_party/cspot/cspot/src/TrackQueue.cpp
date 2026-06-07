#include "TrackQueue.h"
#include <pb_decode.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>

#include "AccessKeyFetcher.h"
#include "ApResolve.h"
#include "BellTask.h"
#include "BellUtils.h"
#include "CDNAudioFile.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "Utils.h"
#include "WrappedSemaphore.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#include "protobuf/metadata.pb.h"
#include "protobuf/storage-resolve.pb.h"

using namespace cspot;

namespace {
std::mutex g_cdnResolveMutex;
std::atomic<int64_t> g_lastCdnResolveMs{0};
std::string g_spclientBaseUrl;

constexpr const char* kSpotifyUserAgent = "Spotify/8.6.84 iOS/15.1 (iPhone11,8)";

std::string spclientBaseUrl() {
  if (!g_spclientBaseUrl.empty()) {
    return g_spclientBaseUrl;
  }
  const std::string hostPort = ApResolve("").fetchFirstSpclientAddress();
  const auto colon = hostPort.find(':');
  if (colon == std::string::npos) {
    g_spclientBaseUrl = "https://" + hostPort;
  } else {
    g_spclientBaseUrl = "https://" + hostPort.substr(0, colon) + ":" +
                        hostPort.substr(colon + 1);
  }
  return g_spclientBaseUrl;
}
}  // namespace

namespace TrackDataUtils {
bool countryListContains(char* countryList, const char* country) {
  uint16_t countryList_length = strlen(countryList);
  for (int x = 0; x < countryList_length; x += 2) {
    if (countryList[x] == country[0] && countryList[x + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

bool doRestrictionsApply(Restriction* restrictions, int count,
                         const char* country) {
  for (int x = 0; x < count; x++) {
    if (restrictions[x].countries_allowed != nullptr) {
      return !countryListContains(restrictions[x].countries_allowed, country);
    }

    if (restrictions[x].countries_forbidden != nullptr) {
      return countryListContains(restrictions[x].countries_forbidden, country);
    }
  }

  return false;
}

bool canPlayTrack(Track& trackInfo, int altIndex, const char* country) {
  if (altIndex < 0) {

  } else {
    for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count;
         x++) {
      if (trackInfo.alternative[altIndex].restriction[x].countries_allowed !=
          nullptr) {
        return countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_allowed,
            country);
      }

      if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden !=
          nullptr) {
        return !countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_forbidden,
            country);
      }
    }
  }
  return true;
}
}  // namespace TrackDataUtils

void TrackInfo::loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbTrack->name);

  if (pbTrack->artist_count > 0) {
    // Handle artist data
    artist = std::string(pbTrack->artist[0].name);
  }

  if (pbTrack->has_album) {
    // Handle album data
    album = std::string(pbTrack->album.name);

    if (pbTrack->album.has_cover_group &&
        pbTrack->album.cover_group.image_count > 0) {
      auto imageId =
          pbArrayToVector(pbTrack->album.cover_group.image[0].file_id);
      imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }
  }

  number = pbTrack->has_number ? pbTrack->number : 0;
  discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
  duration = pbTrack->duration;
}

void TrackInfo::loadPbEpisode(Episode* pbEpisode,
                              const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbEpisode->name);

  if (pbEpisode->covers->image_count > 0) {
    // Handle episode info
    auto imageId = pbArrayToVector(pbEpisode->covers->image[0].file_id);
    imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
  }

  number = pbEpisode->has_number ? pbEpisode->number : 0;
  discNumber = 0;
  duration = pbEpisode->duration;
}

QueuedTrack::QueuedTrack(TrackReference& ref,
                         std::shared_ptr<cspot::Context> ctx,
                         uint32_t requestedPosition)
    : requestedPosition(requestedPosition), ctx(ctx) {
  this->ref = ref;

  loadedSemaphore = std::make_shared<bell::WrappedSemaphore>();
  state = State::QUEUED;
}

QueuedTrack::~QueuedTrack() {
  state = State::FAILED;
  loadedSemaphore->give();

  if (pendingMercuryRequest != 0) {
    ctx->session->unregister(pendingMercuryRequest);
  }

  if (pendingAudioKeyRequest != 0) {
    ctx->session->unregisterAudioKey(pendingAudioKeyRequest);
  }
}

std::shared_ptr<cspot::CDNAudioFile> QueuedTrack::getAudioFile() {
  if (state != State::READY) {
    return nullptr;
  }

  return std::make_shared<cspot::CDNAudioFile>(cdnUrl, audioKey);
}

void QueuedTrack::stepParseMetadata(Track* pbTrack, Episode* pbEpisode) {
  int alternativeCount, filesCount = 0;
  bool canPlay = false;
  AudioFile* selectedFiles = nullptr;

  const char* countryCode = ctx->config.countryCode.c_str();

  if (ref.type == TrackReference::Type::TRACK) {
    CSPOT_LOG(info, "Track name: %s", pbTrack->name);
    CSPOT_LOG(info, "Track duration: %d", pbTrack->duration);

    CSPOT_LOG(debug, "trackInfo.restriction.size() = %d",
              pbTrack->restriction_count);

    // Check if we can play the track, if not, try alternatives
    if (TrackDataUtils::doRestrictionsApply(
            pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
      // Go through alternatives
      for (int x = 0; x < pbTrack->alternative_count; x++) {
        if (!TrackDataUtils::doRestrictionsApply(
                pbTrack->alternative[x].restriction,
                pbTrack->alternative[x].restriction_count, countryCode)) {
          selectedFiles = pbTrack->alternative[x].file;
          filesCount = pbTrack->alternative[x].file_count;
          trackId = pbArrayToVector(pbTrack->alternative[x].gid);
          break;
        }
      }
    } else {
      // We can play the track
      selectedFiles = pbTrack->file;
      filesCount = pbTrack->file_count;
      trackId = pbArrayToVector(pbTrack->gid);
    }

    if (trackId.size() > 0) {
      // Load track information
      trackInfo.loadPbTrack(pbTrack, trackId);
    }
  } else {
    // Handle episodes
    CSPOT_LOG(info, "Episode name: %s", pbEpisode->name);
    CSPOT_LOG(info, "Episode duration: %d", pbEpisode->duration);

    CSPOT_LOG(debug, "episodeInfo.restriction.size() = %d",
              pbEpisode->restriction_count);

    // Check if we can play the episode
    if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction,
                                             pbEpisode->restriction_count,
                                             countryCode)) {
      selectedFiles = pbEpisode->file;
      filesCount = pbEpisode->file_count;
      trackId = pbArrayToVector(pbEpisode->gid);

      // Load track information
      trackInfo.loadPbEpisode(pbEpisode, trackId);
    }
  }

  // Find playable file
  for (int x = 0; x < filesCount; x++) {
    CSPOT_LOG(debug, "File format: %d", selectedFiles[x].format);
    if (selectedFiles[x].format == ctx->config.audioFormat) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      break;  // If file found stop searching
    }

    // Fallback to OGG Vorbis 96kbps
    if (fileId.size() == 0 &&
        selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
    }
  }

  // No viable files found for playback
  if (fileId.size() == 0) {
    CSPOT_LOG(info, "File not available for playback");

    // no alternatives for song
    state = State::FAILED;
    loadedSemaphore->give();
    return;
  }

  // Assign track identifier
  identifier = bytesToHexString(fileId);

  state = State::KEY_REQUIRED;
}

void QueuedTrack::stepLoadAudioFile(
    std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  // Request audio key
  this->pendingAudioKeyRequest = ctx->session->requestAudioKey(
      trackId, fileId,
      [this, &trackListMutex, updateSemaphore](
          bool success, const std::vector<uint8_t>& audioKey) {
        std::scoped_lock lock(trackListMutex);

        if (success) {
          CSPOT_LOG(info, "Got audio key");
          this->audioKey =
              std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());

          state = State::CDN_REQUIRED;
        } else {
          CSPOT_LOG(error, "Failed to get audio key");
          state = State::FAILED;
          loadedSemaphore->give();
        }
        updateSemaphore->give();
      });

  state = State::PENDING_KEY;
}

void QueuedTrack::stepLoadCDNUrl(const std::string& accessKey) {
  if (accessKey.size() == 0) {
    return;
  }

  CSPOT_LOG(info, "Fetching CDN URL for %s...", identifier.c_str());

  const std::string country =
      ctx->config.countryCode.empty() ? "US" : ctx->config.countryCode;
  const uint32_t salt = static_cast<uint32_t>(rand());
  const std::string requestUrl = string_format(
      "%s/storage-resolve/files/audio/interactive/%s?product=0&country=%s&"
      "salt=%u",
      spclientBaseUrl().c_str(), bytesToHexString(fileId).c_str(),
      country.c_str(), salt);

  const bell::HTTPClient::Headers headers = {
      {"Authorization", "Bearer " + accessKey},
      {"User-Agent", kSpotifyUserAgent},
      {"Accept", "application/x-protobuf"},
  };

  std::lock_guard<std::mutex> lock(g_cdnResolveMutex);

  for (int attempt = 0; attempt < 8; attempt++) {
    if (attempt > 0) {
      const int delayMs = std::min(30000, 2000 * attempt);
      CSPOT_LOG(info, "CDN resolve retry in %d ms (attempt %d)", delayMs,
                attempt + 1);
      BELL_SLEEP_MS(delayMs);
    } else if (ctx && ctx->timeProvider) {
      const int64_t now = ctx->timeProvider->getSyncedTimestamp();
      const int64_t prev = g_lastCdnResolveMs.load();
      if (prev > 0 && now > prev && (now - prev) < 1500) {
        BELL_SLEEP_MS(static_cast<int>(1500 - (now - prev)));
      }
      g_lastCdnResolveMs = now;
    }

    try {
      auto req = bell::HTTPClient::get(requestUrl, headers);
      const int status = req->statusCode();

      if (status == 429) {
        CSPOT_LOG(error, "CDN resolve rate limited (429), attempt %d",
                  attempt + 1);
        continue;
      }

      if (status != 200) {
        CSPOT_LOG(error, "CDN resolve HTTP %d", status);
        continue;
      }

      const std::string_view result = req->body();
      if (result.empty()) {
        CSPOT_LOG(error, "CDN resolve empty body");
        continue;
      }

      static spotify_download_proto_StorageResolveResponse resolveResponse =
          spotify_download_proto_StorageResolveResponse_init_zero;
      pb_release(spotify_download_proto_StorageResolveResponse_fields,
                 &resolveResponse);
      std::vector<uint8_t> responseBytes(result.begin(), result.end());
      pbDecode(resolveResponse,
               spotify_download_proto_StorageResolveResponse_fields,
               responseBytes);

      if (resolveResponse.result !=
              spotify_download_proto_StorageResolveResponse_Result_CDN ||
          resolveResponse.cdnurl_count == 0) {
        CSPOT_LOG(error, "CDN resolve invalid response (result=%d urls=%d)",
                  resolveResponse.result, resolveResponse.cdnurl_count);
        pb_release(spotify_download_proto_StorageResolveResponse_fields,
                   &resolveResponse);
        continue;
      }

      cdnUrl = resolveResponse.cdnurl[0];
      pb_release(spotify_download_proto_StorageResolveResponse_fields,
                 &resolveResponse);

      CSPOT_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
      state = State::READY;
      loadedSemaphore->give();
      return;
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "CDN resolve error: %s", e.what());
    } catch (...) {
      CSPOT_LOG(error, "CDN resolve error: unknown");
    }
  }

  CSPOT_LOG(error, "Cannot fetch CDN URL after retries");
  state = State::FAILED;
  loadedSemaphore->give();
}

void QueuedTrack::expire() {
  if (state != State::QUEUED) {
    state = State::FAILED;
    loadedSemaphore->give();
  }
}

void QueuedTrack::stepLoadMetadata(
    Track* pbTrack, Episode* pbEpisode, std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {

  // Prepare request ID
  std::string requestUrl = string_format(
      "hm://metadata/3/%s/%s",
      ref.type == TrackReference::Type::TRACK ? "track" : "episode",
      bytesToHexString(ref.gid).c_str());

  auto responseHandler = [this, pbTrack, pbEpisode, &trackListMutex,
                          updateSemaphore](MercurySession::Response& res) {
    std::scoped_lock lock(trackListMutex);

    if (res.parts.size() == 0) {
      // Invalid metadata, cannot proceed
      state = State::FAILED;
      updateSemaphore->give();
      loadedSemaphore->give();
      return;
    }

    // Parse the metadata
    if (ref.type == TrackReference::Type::TRACK) {
      pb_release(Track_fields, pbTrack);
      pbDecode(*pbTrack, Track_fields, res.parts[0]);
    } else {
      pb_release(Episode_fields, pbEpisode);
      pbDecode(*pbEpisode, Episode_fields, res.parts[0]);
    }

    // Parse received metadata
    stepParseMetadata(pbTrack, pbEpisode);

    updateSemaphore->give();
  };
  // Execute the request
  pendingMercuryRequest = ctx->session->execute(
      MercurySession::RequestType::GET, requestUrl, responseHandler);

  // Set the state to pending
  state = State::PENDING_META;
}

TrackQueue::TrackQueue(std::shared_ptr<cspot::Context> ctx,
                       std::shared_ptr<cspot::PlaybackState> state)
    : bell::Task("CSpotTrackQueue", 1024 * 32, 2, 1),
      playbackState(state),
      ctx(ctx) {
  accessKeyFetcher = std::make_shared<cspot::AccessKeyFetcher>(ctx);
  processSemaphore = std::make_shared<bell::WrappedSemaphore>();
  playableSemaphore = std::make_shared<bell::WrappedSemaphore>();

  // Assign encode callback to track list
  playbackState->innerFrame.state.track.funcs.encode =
      &TrackReference::pbEncodeTrackList;
  playbackState->innerFrame.state.track.arg = &currentTracks;
  pbTrack = Track_init_zero;
  pbEpisode = Episode_init_zero;

  // Start the task
  startTask();
};

TrackQueue::~TrackQueue() {
  stopTask();

  std::scoped_lock lock(tracksMutex);

  pb_release(Track_fields, &pbTrack);
  pb_release(Episode_fields, &pbEpisode);
}

TrackInfo TrackQueue::getTrackInfo(std::string_view identifier) {
  for (auto& track : preloadedTracks) {
    if (track->identifier == identifier)
      return track->trackInfo;
  }
  return TrackInfo{};
}

bool TrackQueue::getCurrentPlayingTrack(std::shared_ptr<QueuedTrack>& out) {
  std::scoped_lock lock(tracksMutex);
  if (currentTracksIndex < 0 ||
      currentTracksIndex >= static_cast<int>(currentTracks.size())) {
    return false;
  }
  const TrackReference& cur = currentTracks[currentTracksIndex];
  for (const auto& track : preloadedTracks) {
    if (track->state == QueuedTrack::State::READY && track->ref == cur) {
      out = track;
      return true;
    }
  }
  // Fallback: head of preload queue is usually the active track.
  if (!preloadedTracks.empty() &&
      preloadedTracks.front()->state == QueuedTrack::State::READY &&
      preloadedTracks.front()->ref == cur) {
    out = preloadedTracks.front();
    return true;
  }
  return false;
}

bool TrackQueue::peekHeadTrackInfo(TrackInfo& out) {
  std::scoped_lock lock(tracksMutex);
  if (preloadedTracks.empty() || currentTracksIndex < 0) {
    return false;
  }
  const auto& head = preloadedTracks.front();
  if (head->trackInfo.name.empty() && head->trackInfo.artist.empty()) {
    return false;
  }
  out = head->trackInfo;
  return true;
}

void TrackQueue::runTask() {
  isRunning = true;

  std::scoped_lock lock(runningMutex);

  std::deque<std::shared_ptr<QueuedTrack>> trackQueue;

  while (isRunning) {
    processSemaphore->twait(100);

    // Make sure we have the newest access key
    accessKey = accessKeyFetcher->getAccessKey();

    int loadedIndex = currentTracksIndex;

    // No tracks loaded yet
    if (loadedIndex < 0) {
      continue;
    } else {
      std::scoped_lock lock(tracksMutex);

      trackQueue = preloadedTracks;
    }

    for (auto& track : trackQueue) {
      if (track) {
        this->processTrack(track);
      }
    }
  }
}

void TrackQueue::stopTask() {
  if (isRunning) {
    isRunning = false;
    processSemaphore->give();
    std::scoped_lock lock(runningMutex);
  }
}

std::shared_ptr<QueuedTrack> TrackQueue::consumeTrack(
    std::shared_ptr<QueuedTrack> prevTrack, int& offset) {
  std::scoped_lock lock(tracksMutex);

  if (currentTracksIndex == -1 || currentTracksIndex >= currentTracks.size()) {
    return nullptr;
  }

  // No previous track, return head
  if (prevTrack == nullptr) {
    offset = 0;

    return preloadedTracks[0];
  }

  // if (currentTracksIndex + preloadedTracks.size() >= currentTracks.size()) {
  //   offset = -1;

  //   // Last track in queue
  //   return nullptr;
  // }

  auto prevTrackIter =
      std::find(preloadedTracks.begin(), preloadedTracks.end(), prevTrack);

  if (prevTrackIter != preloadedTracks.end()) {
    // Get offset of next track
    offset = prevTrackIter - preloadedTracks.begin() + 1;
  } else {
    offset = 0;
  }

  if (offset >= preloadedTracks.size()) {
    // Last track in preloaded queue
    return nullptr;
  }

  // Return the current track
  return preloadedTracks[offset];
}

void TrackQueue::processTrack(std::shared_ptr<QueuedTrack> track) {
  switch (track->state) {
    case QueuedTrack::State::QUEUED:
      track->stepLoadMetadata(&pbTrack, &pbEpisode, tracksMutex,
                              processSemaphore);
      break;
    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(tracksMutex, processSemaphore);
      break;
    case QueuedTrack::State::CDN_REQUIRED: {
      std::shared_ptr<QueuedTrack> head;
      {
        std::scoped_lock lock(tracksMutex);
        if (!preloadedTracks.empty()) {
          head = preloadedTracks.front();
        }
      }
      if (track != head) {
        break;
      }
      track->stepLoadCDNUrl(accessKey);

      if (track->state == QueuedTrack::State::READY) {
        if (preloadedTracks.size() < MAX_TRACKS_PRELOAD) {
          // Queue a new track to preload
          queueNextTrack(preloadedTracks.size());
        }
      }
      break;
    }
    default:
      // Do not perform any action
      break;
  }
}

bool TrackQueue::queueNextTrack(int offset, uint32_t positionMs) {
  const int requestedRefIndex = offset + currentTracksIndex;

  if (requestedRefIndex < 0 || requestedRefIndex >= currentTracks.size()) {
    return false;
  }

  // in case we re-queue current track, make sure position is updated (0)
  if (offset == 0 && preloadedTracks.size() &&
      preloadedTracks[0]->ref == currentTracks[currentTracksIndex]) {
    preloadedTracks.pop_front();
  }

  if (offset <= 0) {
    preloadedTracks.push_front(std::make_shared<QueuedTrack>(
        currentTracks[requestedRefIndex], ctx, positionMs));
  } else {
    preloadedTracks.push_back(std::make_shared<QueuedTrack>(
        currentTracks[requestedRefIndex], ctx, positionMs));
  }

  return true;
}

bool TrackQueue::skipTrack(SkipDirection dir, bool expectNotify) {
  bool skipped = true;
  std::scoped_lock lock(tracksMutex);

  if (dir == SkipDirection::PREV) {
    uint64_t position =
        !playbackState->innerFrame.state.has_position_ms
            ? 0
            : playbackState->innerFrame.state.position_ms +
                  ctx->timeProvider->getSyncedTimestamp() -
                  playbackState->innerFrame.state.position_measured_at;

    if (currentTracksIndex > 0 && position < 3000) {
      queueNextTrack(-1);

      if (preloadedTracks.size() > MAX_TRACKS_PRELOAD) {
        preloadedTracks.pop_back();
      }

      currentTracksIndex--;
    } else {
      queueNextTrack(0);
    }
  } else {
    if (playbackState->innerFrame.state.shuffle &&
        currentTracks.size() > 1) {
      preloadedTracks.clear();
      int newIndex = currentTracksIndex;
      for (int attempt = 0; attempt < 8 && newIndex == currentTracksIndex;
           attempt++) {
        newIndex = static_cast<int>(rand()) %
                   static_cast<int>(currentTracks.size());
      }
      currentTracksIndex = newIndex;
      queueNextTrack(0);
      skipped = true;
    } else if (currentTracks.size() > currentTracksIndex + 1) {
      preloadedTracks.pop_front();

      if (!queueNextTrack(preloadedTracks.size() + 1)) {
        CSPOT_LOG(info, "Failed to queue next track");
      }

      currentTracksIndex++;
    } else {
      skipped = false;
    }
  }

  if (skipped) {
    // Update frame data
    playbackState->innerFrame.state.playing_track_index = currentTracksIndex;

    if (expectNotify) {
      // Reset position to zero
      notifyPending = true;
    }
  }

  return skipped;
}

bool TrackQueue::hasTracks() {
  std::scoped_lock lock(tracksMutex);

  return currentTracks.size() > 0;
}

bool TrackQueue::isFinished() {
  std::scoped_lock lock(tracksMutex);
  return currentTracks.size() > 0 &&
         currentTracksIndex >= static_cast<int>(currentTracks.size()) - 1;
}

bool TrackQueue::restartFromStart() {
  std::scoped_lock lock(tracksMutex);
  if (currentTracks.empty()) {
    return false;
  }
  currentTracksIndex = 0;
  preloadedTracks.clear();
  queueNextTrack(0, 0);
  playbackState->innerFrame.state.playing_track_index = 0;
  notifyPending = true;
  return true;
}

bool TrackQueue::updateTracks(uint32_t requestedPosition, bool initial) {
  std::scoped_lock lock(tracksMutex);
  bool cleared = true;

  // Copy requested track list
  currentTracks = playbackState->remoteTracks;
  currentTracksIndex = playbackState->innerFrame.state.playing_track_index;

  if (initial) {
    // Clear preloaded tracks
    preloadedTracks.clear();

    if (currentTracksIndex < currentTracks.size()) {
      // Push a song on the preloaded queue
      queueNextTrack(0, requestedPosition);
    }

    // We already updated track meta, mark it
    notifyPending = true;

    playableSemaphore->give();
  } else if (preloadedTracks[0]->loading) {
    // try to not re-load track if we are still loading it

    // remove everything except first track
    preloadedTracks.erase(preloadedTracks.begin() + 1, preloadedTracks.end());

    // Push a song on the preloaded queue
    CSPOT_LOG(info, "Keeping current track %d", currentTracksIndex);
    queueNextTrack(1);

    cleared = false;
  } else {
    // Clear preloaded tracks
    preloadedTracks.clear();

    // Push a song on the preloaded queue
    CSPOT_LOG(info, "Re-loading current track");
    queueNextTrack(0, requestedPosition);
  }

  return cleared;
}
