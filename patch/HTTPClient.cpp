#include "HTTPClient.h"

#include <string.h>   // for memcpy
#include <algorithm>  // for transform
#include <cassert>    // for assert
#include <cctype>     // for tolower
#include <ostream>    // for operator<<, basic_ostream
#include <stdexcept>  // for runtime_error

#include "BellSocket.h"  // for bell

using namespace bell;

void HTTPClient::Response::connect(const std::string& url) {
  urlParser = bell::URLParser::parse(url);

  this->socketStream.open(urlParser.host, urlParser.port,
                          urlParser.schema == "https");
}

HTTPClient::Response::~Response() {
  if (this->socketStream.isOpen()) {
    this->socketStream.close();
  }
}

void HTTPClient::Response::rawRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<uint8_t>& content,
                                      Headers& headers) {
  urlParser = bell::URLParser::parse(url);

  const char* reqEnd = "\r\n";

  // Range/streaming requests (e.g. CDN audio) reuse a single connection for the
  // header, footer and every subsequent chunk, so they MUST stay keep-alive.
  // Plain API body reads have no Range header; on Vita we close those so the
  // read-until-EOF body() path terminates even without a Content-Length.
  bool hasRange = false;
  for (auto& header : headers) {
    std::string key = header.first;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (key == "range") {
      hasRange = true;
      break;
    }
  }

  socketStream << method << " " << urlParser.path << " HTTP/1.1" << reqEnd;
  socketStream << "Host: " << urlParser.host << ":" << urlParser.port << reqEnd;
#ifdef VITA
  socketStream << "Connection: " << (hasRange ? "keep-alive" : "close")
               << reqEnd;
#else
  socketStream << "Connection: keep-alive" << reqEnd;
#endif
  socketStream << "Accept: */*" << reqEnd;

  if (content.size() > 0) {
    socketStream << "Content-Length: " << content.size() << reqEnd;
  }

  for (auto& header : headers) {
    socketStream << header.first << ": " << header.second << reqEnd;
  }

  socketStream << reqEnd;

  if (content.size() > 0) {
    socketStream.write((const char*)content.data(), content.size());
  }

  socketStream.flush();

  readResponseHeaders();
}

void HTTPClient::Response::readResponseHeaders() {
  char *method, *path;
  const char* msgPointer;

  size_t msgLen;
  int pret, minorVersion, status;

  size_t prevbuflen = 0, numHeaders;
  this->httpBufferAvailable = 0;
  this->httpStatus = 0;

  while (1) {
    socketStream.getline((char*)httpBuffer.data() + httpBufferAvailable,
                         httpBuffer.size() - httpBufferAvailable);

    prevbuflen = httpBufferAvailable;
    httpBufferAvailable += socketStream.gcount();

    memcpy(httpBuffer.data() + httpBufferAvailable - 2, "\r\n", 2);

    numHeaders = sizeof(phResponseHeaders) / sizeof(phResponseHeaders[0]);

    pret =
        phr_parse_response((const char*)httpBuffer.data(), httpBufferAvailable,
                           &minorVersion, &status, &msgPointer, &msgLen,
                           phResponseHeaders, &numHeaders, prevbuflen);

    if (pret > 0) {
      httpStatus = status;
      break;
    } else if (pret == -1)
      throw std::runtime_error("Cannot parse http response");

    assert(pret == -2);
    if (httpBufferAvailable == httpBuffer.size())
      throw std::runtime_error("Response too large");
  }

  this->responseHeaders = {};

  for (int headerIndex = 0; headerIndex < numHeaders; headerIndex++) {
    this->responseHeaders.push_back(
        ValueHeader{std::string(phResponseHeaders[headerIndex].name,
                                phResponseHeaders[headerIndex].name_len),
                    std::string(phResponseHeaders[headerIndex].value,
                                phResponseHeaders[headerIndex].value_len)});
  }

  std::string contentLengthValue = std::string(header("content-length"));
  if (contentLengthValue.size() > 0) {
    this->hasContentSize = true;
    this->contentSize = std::stoi(contentLengthValue);
  }
}

void HTTPClient::Response::get(const std::string& url, Headers headers) {
  std::string method = "GET";
  return this->rawRequest(url, method, {}, headers);
}

void HTTPClient::Response::post(const std::string& url, Headers headers,
                                const std::vector<uint8_t>& body) {
  std::string method = "POST";
  return this->rawRequest(url, method, body, headers);
}

size_t HTTPClient::Response::contentLength() {
  return contentSize;
}

std::string_view HTTPClient::Response::header(const std::string& headerName) {
  for (auto& header : this->responseHeaders) {
    std::string headerValue = header.first;
    std::transform(headerValue.begin(), headerValue.end(), headerValue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (headerName == headerValue) {
      return header.second;
    }
  }

  return "";
}

size_t HTTPClient::Response::totalLength() {
  auto rangeHeader = header("content-range");

  if (rangeHeader.find("/") != std::string::npos) {
    return std::stoi(
        std::string(rangeHeader.substr(rangeHeader.find("/") + 1)));
  }

  return this->contentLength();
}

void HTTPClient::Response::readRawBody() {
  if (!rawBody.empty()) {
    return;
  }

  if (hasContentSize && contentSize > 0) {
    rawBody.assign(contentSize, 0);
    socketStream.read(reinterpret_cast<char*>(rawBody.data()),
                    static_cast<std::streamsize>(contentSize));
    return;
  }

  // No Content-Length (common for Spotify API): read until connection closes.
  std::vector<uint8_t> chunk(4096);
  while (socketStream.good()) {
    socketStream.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
    const std::streamsize n = socketStream.gcount();
    if (n <= 0) {
      break;
    }
    rawBody.insert(rawBody.end(), chunk.begin(), chunk.begin() + n);
    if (rawBody.size() > 512 * 1024) {
      throw std::runtime_error("HTTP body too large");
    }
  }
}

std::string_view HTTPClient::Response::body() {
  readRawBody();
  return std::string_view((char*)rawBody.data(), rawBody.size());
}

std::vector<uint8_t> HTTPClient::Response::bytes() {
  readRawBody();
  return rawBody;
}
