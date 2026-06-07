#include "CliFile.h"

#include <cstdio>
#include <fstream>

bool CliFile::readFile(const std::string& filename, std::string& fileContent) {
    std::ifstream configFile(filename);
    if (!configFile.is_open()) {
        fileContent.clear();
        return false;
    }
    fileContent.assign(std::istreambuf_iterator<char>(configFile),
                       std::istreambuf_iterator<char>());
    return true;
}

bool CliFile::writeFile(const std::string& filename, const std::string& fileContent) {
    FILE* fd = fopen(filename.c_str(), "w");
    if (fd == nullptr) {
        return false;
    }
    fwrite(fileContent.c_str(), fileContent.size(), 1, fd);
    fclose(fd);
    return true;
}
