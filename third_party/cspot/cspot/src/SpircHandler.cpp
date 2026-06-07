#include "SpircHandler.h"

#include <cstdint>      // for uint8_t
#include <cstdlib>      // for realloc
#include <cstring>      // for strcpy
#include <memory>       // for shared_ptr, make_unique, unique_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move

#include "BellLogger.h"      // for AbstractLogger
#include "CSpotContext.h"    // for Context::ConfigState, Context (ptr only)
#include "Logger.h"          // for CSPOT_LOG
#include "MercurySession.h"  // for MercurySession, MercurySession::Response
#include "NanoPBHelper.h"    // for pbDecode
#include "Packet.h"          // for cspot
#include "PlaybackState.h"   // for PlaybackState, PlaybackState::State
#include "TrackPlayer.h"     // for TrackPlayer
#include "TrackQueue.h"
#include "TrackReference.h"     // for TrackReference
#include "Utils.h"              // for stringHexToBytes
#include "pb_decode.h"          // for pb_release
#include "protobuf/spirc.pb.h"  // for Frame, State, Frame_fields, MessageTy...

using namespace cspot;

SpircHandler::SpircHandler(std::shared_ptr<cspot::Context> ctx) {
  this->playbackState = std::make_shared<PlaybackState>(ctx);
  this->trackQueue = std::make_shared<cspot::TrackQueue>(ctx, playbackState);

  auto EOFCallback = [this]() {
    if (trackQueue->isFinished()) {
      if (playbackState->innerFrame.state.repeat && trackQueue->hasTracks()) {
        trackQueue->restartFromStart();
        trackPlayer->resetState(true);
      } else {
        sendEvent(EventType::DEPLETED);
      }
    }
  };

  auto trackLoadedCallback = [this](std::shared_ptr<QueuedTrack> track,
                                    bool paused = false) {
    playbackState->setPlaybackState(paused ? PlaybackState::State::Paused
                                           : PlaybackState::State::Playing);
    playbackState->updatePositionMs(track->requestedPosition);

    this->notify();

    sendEvent(EventType::TRACK_INFO, track->trackInfo);
    sendEvent(EventType::PLAYBACK_START, (int)track->requestedPosition);
    sendEvent(EventType::PLAY_PAUSE, paused);
  };

  this->ctx = ctx;
  this->trackPlayer = std::make_shared<TrackPlayer>(
      ctx, trackQueue, EOFCallback, trackLoadedCallback);

  // Subscribe to mercury on session ready
  ctx->session->setConnectedHandler([this]() { this->subscribeToMercury(); });
}

void SpircHandler::subscribeToMercury() {
  auto responseLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;

    sendCmd(MessageType_kMessageTypeHello);
    CSPOT_LOG(debug, "Sent kMessageTypeHello!");

    // Assign country code
    this->ctx->config.countryCode = this->ctx->session->getCountryCode();
  };
  auto subscriptionLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;
    CSPOT_LOG(debug, "Received subscription response");

    this->handleFrame(res.parts[0]);
  };

  ctx->session->executeSubscription(
      MercurySession::RequestType::SUB,
      "hm://remote/user/" + ctx->config.username + "/", responseLambda,
      subscriptionLambda);
}

void SpircHandler::loadTrackFromURI(const std::string& uri) {}

void SpircHandler::loadAndPlayTracks(const std::vector<std::string>& trackUris,
                                     const std::string& contextUri,
                                     int startIndex) {
  if (trackUris.empty()) {
    CSPOT_LOG(info, "loadAndPlayTracks: no tracks to play");
    return;
  }

  // The Connect state frame (sent on notify) carries the whole track list.
  // Real controllers send a bounded window, not thousands of tracks; sending
  // too many overflows the frame and the Mercury session drops. Window the
  // list around the requested start track.
  constexpr int kMaxWindow = 64;
  if (startIndex < 0 || startIndex >= static_cast<int>(trackUris.size())) {
    startIndex = 0;
  }

  int windowStart = startIndex;
  int windowEnd = static_cast<int>(trackUris.size());
  if (windowEnd - windowStart > kMaxWindow) {
    windowEnd = windowStart + kMaxWindow;
  }

  this->trackPlayer->start();

  // Build the queue directly (no remote controller frame involved).
  playbackState->remoteTracks.clear();
  for (int i = windowStart; i < windowEnd; i++) {
    TrackReference ref;
    ref.uri = trackUris[i];
    ref.context = contextUri;
    ref.decodeURI();
    playbackState->remoteTracks.push_back(std::move(ref));
  }

  // We rebased the list to start at windowStart, so play from its head.
  startIndex = 0;

  // Reflect the context in our own state so notifications look correct.
  playbackState->innerFrame.state.context_uri = static_cast<char*>(
      realloc(playbackState->innerFrame.state.context_uri,
              contextUri.size() + 1));
  strcpy(playbackState->innerFrame.state.context_uri, contextUri.c_str());

  playbackState->innerFrame.state.has_playing_track_index = true;
  playbackState->innerFrame.state.playing_track_index = startIndex;

  // Become the active device and start playing. notify() publishes this to
  // Spotify so no external app needs to transfer playback to the Vita.
  playbackState->setActive(true);
  playbackState->updatePositionMs(0);
  playbackState->setPlaybackState(PlaybackState::State::Playing);

  trackQueue->updateTracks(0, true);

  this->notify();

  trackPlayer->resetState();

  CSPOT_LOG(info, "loadAndPlayTracks: queued %d tracks (window %d..%d of %d)",
            (int)playbackState->remoteTracks.size(), windowStart, windowEnd,
            (int)trackUris.size());
}

void SpircHandler::notifyAudioEnded() {
  playbackState->updatePositionMs(0);
  notify();
  trackPlayer->resetState(true);
}

void SpircHandler::notifyAudioReachedPlayback() {
  int offset = 0;

  // get HEAD track
  auto currentTrack = trackQueue->consumeTrack(nullptr, offset);

  // Do not execute when meta is already updated
  if (trackQueue->notifyPending) {
    trackQueue->notifyPending = false;

    playbackState->updatePositionMs(currentTrack->requestedPosition);

    // Reset position in queued track
    currentTrack->requestedPosition = 0;
  } else {
    trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT, false);
    playbackState->updatePositionMs(0);

    // we moved to next track, re-acquire currentTrack again
    currentTrack = trackQueue->consumeTrack(nullptr, offset);
  }

  this->notify();

  sendEvent(EventType::TRACK_INFO, currentTrack->trackInfo);
}

void SpircHandler::updatePositionMs(uint32_t position) {
  playbackState->updatePositionMs(position);
  notify();
}

void SpircHandler::disconnect() {
  this->trackQueue->stopTask();
  this->trackPlayer->stop();
  this->ctx->session->disconnect();
}

void SpircHandler::handleFrame(std::vector<uint8_t>& data) {
  // Decode received spirc frame
  playbackState->decodeRemoteFrame(data);

  switch (playbackState->remoteFrame.typ) {
    case MessageType_kMessageTypeNotify: {
      CSPOT_LOG(debug, "Notify frame");

      // Pause the playback if another player took control
      if (playbackState->isActive() &&
          playbackState->remoteFrame.device_state.is_active) {
        CSPOT_LOG(debug, "Another player took control, pausing playback");
        playbackState->setActive(false);

        this->trackPlayer->stop();
        sendEvent(EventType::DISC);
      }
      break;
    }
    case MessageType_kMessageTypeSeek: {
      this->trackPlayer->seekMs(playbackState->remoteFrame.position);

      playbackState->updatePositionMs(playbackState->remoteFrame.position);

      notify();

      sendEvent(EventType::SEEK, (int)playbackState->remoteFrame.position);
      break;
    }
    case MessageType_kMessageTypeVolume:
      playbackState->setVolume(playbackState->remoteFrame.volume);
      this->notify();
      sendEvent(EventType::VOLUME, (int)playbackState->remoteFrame.volume);
      break;
    case MessageType_kMessageTypePause:
      setPause(true);
      break;
    case MessageType_kMessageTypePlay:
      setPause(false);
      break;
    case MessageType_kMessageTypeNext:
      if (nextSong()) {
        sendEvent(EventType::NEXT);
      }
      break;
    case MessageType_kMessageTypePrev:
      if (previousSong()) {
        sendEvent(EventType::PREV);
      }
      break;
    case MessageType_kMessageTypeLoad: {
      this->trackPlayer->start();

      CSPOT_LOG(debug, "Load frame %d!", playbackState->remoteTracks.size());

      if (playbackState->remoteTracks.size() == 0) {
        CSPOT_LOG(info, "No tracks in frame, stopping playback");
        break;
      }

      playbackState->setActive(true);

      playbackState->updatePositionMs(playbackState->remoteFrame.position);
      playbackState->setPlaybackState(PlaybackState::State::Playing);

      playbackState->syncWithRemote();

      // Update track list in case we have a new one
      trackQueue->updateTracks(playbackState->remoteFrame.state.position_ms,
                               true);

      this->notify();

      // Stop the current track, if any
      trackPlayer->resetState();
      break;
    }
    case MessageType_kMessageTypeReplace: {
      CSPOT_LOG(debug, "Got replace frame %d",
                playbackState->remoteTracks.size());
      playbackState->syncWithRemote();

      // 1st track is the current one, but update the position
      bool cleared = trackQueue->updateTracks(
          playbackState->remoteFrame.state.position_ms +
              ctx->timeProvider->getSyncedTimestamp() -
              playbackState->innerFrame.state.position_measured_at,
          false);

      this->notify();

      // need to re-load all if streaming track is completed
      if (cleared) {
        sendEvent(EventType::FLUSH);
        trackPlayer->resetState();
      }
      break;
    }
    case MessageType_kMessageTypeShuffle: {
      CSPOT_LOG(debug, "Got shuffle frame");
      if (playbackState->remoteFrame.state.has_shuffle) {
        playbackState->innerFrame.state.shuffle =
            playbackState->remoteFrame.state.shuffle;
        playbackState->innerFrame.state.has_shuffle = true;
      }
      this->notify();
      break;
    }
    case MessageType_kMessageTypeRepeat: {
      CSPOT_LOG(debug, "Got repeat frame");
      if (playbackState->remoteFrame.state.has_repeat) {
        playbackState->innerFrame.state.repeat =
            playbackState->remoteFrame.state.repeat;
        playbackState->innerFrame.state.has_repeat = true;
      }
      this->notify();
      break;
    }
    default:
      break;
  }
}

void SpircHandler::setRemoteVolume(int volume) {
  playbackState->setVolume(volume);
  notify();
}

void SpircHandler::notify() {
  this->sendCmd(MessageType_kMessageTypeNotify);
}

bool SpircHandler::skipSong(TrackQueue::SkipDirection dir) {
  bool skipped = trackQueue->skipTrack(dir);

  // Reset track state
  trackPlayer->resetState(!skipped);

  // send NEXT or PREV event only when successful
  return skipped;
}

bool SpircHandler::nextSong() {
  return skipSong(TrackQueue::SkipDirection::NEXT);
}

bool SpircHandler::previousSong() {
  return skipSong(TrackQueue::SkipDirection::PREV);
}

void SpircHandler::toggleShuffle() {
  playbackState->innerFrame.state.shuffle =
      !playbackState->innerFrame.state.shuffle;
  playbackState->innerFrame.state.has_shuffle = true;
  sendCmd(MessageType_kMessageTypeShuffle);
}

void SpircHandler::toggleRepeat() {
  playbackState->innerFrame.state.repeat =
      !playbackState->innerFrame.state.repeat;
  playbackState->innerFrame.state.has_repeat = true;
  sendCmd(MessageType_kMessageTypeRepeat);
}

bool SpircHandler::isShuffleOn() const {
  return playbackState->innerFrame.state.shuffle;
}

bool SpircHandler::isRepeatOn() const {
  return playbackState->innerFrame.state.repeat;
}

uint32_t SpircHandler::getPositionMs() const {
  const auto &s = playbackState->innerFrame.state;
  if (!s.has_position_ms) {
    return 0;
  }
  // Only advance position while actively playing; when paused the clock is frozen
  // in setPlaybackState(Paused).
  if (!s.has_status || s.status != PlayStatus_kPlayStatusPlay ||
      !s.has_position_measured_at) {
    return s.position_ms;
  }
  const uint64_t now = ctx->timeProvider->getSyncedTimestamp();
  return static_cast<uint32_t>(s.position_ms + (now - s.position_measured_at));
}

void SpircHandler::seekToMs(uint32_t ms) {
  trackPlayer->seekMs(ms);
  playbackState->updatePositionMs(ms);
  notify();
}

std::shared_ptr<TrackPlayer> SpircHandler::getTrackPlayer() {
  return this->trackPlayer;
}

void SpircHandler::sendCmd(MessageType typ) {
  // Serialize current player state
  auto encodedFrame = playbackState->encodeCurrentFrame(typ);

  auto responseLambda = [=](MercurySession::Response& res) {
  };
  auto parts = MercurySession::DataParts({encodedFrame});
  ctx->session->execute(MercurySession::RequestType::SEND,
                        "hm://remote/user/" + ctx->config.username + "/",
                        responseLambda, parts);
}
void SpircHandler::setEventHandler(EventHandler handler) {
  this->eventHandler = handler;
}

void SpircHandler::setPause(bool isPaused) {
  if (isPaused) {
    CSPOT_LOG(debug, "External pause command");
    playbackState->setPlaybackState(PlaybackState::State::Paused);
  } else {
    CSPOT_LOG(debug, "External play command");

    playbackState->setPlaybackState(PlaybackState::State::Playing);
  }
  notify();
  sendEvent(EventType::PLAY_PAUSE, isPaused);
}

void SpircHandler::sendEvent(EventType type) {
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = {};
  eventHandler(std::move(event));
}

void SpircHandler::sendEvent(EventType type, EventData data) {
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = data;
  eventHandler(std::move(event));
}
