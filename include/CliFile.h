#pragma once

#include <string>

class CliFile {
 public:
    bool readFile(const std::string& filename, std::string& fileContent);
    bool writeFile(const std::string& filename, const std::string& fileContent);
};
