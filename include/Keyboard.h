#pragma once

#include <string>

namespace Keyboard {
    bool initSystem();
    std::string GetText(const std::string &title, bool password = false);
    const char *getLastError();
}  // namespace Keyboard
