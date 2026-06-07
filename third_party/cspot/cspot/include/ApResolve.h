#pragma once

#include <string>  // for string
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#endif

namespace cspot {
class ApResolve {
 public:
  ApResolve(std::string apOverride);

  /**
   * @brief Connects to spotify's servers and returns first valid ap address
   * @returns std::string Address in form of url:port
   */
  std::string fetchFirstApAddress();

  /** First spclient host:port from apresolve (e.g. gew1-spclient.spotify.com:443). */
  std::string fetchFirstSpclientAddress();

 private:
  std::string apOverride;
};
}  // namespace cspot
