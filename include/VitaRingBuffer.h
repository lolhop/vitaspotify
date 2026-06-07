#pragma once

#include <mutex>
#include <cstdint>
#include <cstring>

class VitaRingBuffer {
 public:
    explicit VitaRingBuffer(size_t dataCapacity);
    ~VitaRingBuffer();

    size_t size() const { return dataSize; }
    size_t capacity() const { return dataCapacity; }
    size_t write(const uint8_t* data, size_t bytes);
    size_t read(uint8_t* data, size_t bytes);
    void emptyBuffer();

 private:
    std::mutex bufferMutex;
    size_t begIndex = 0;
    size_t endIndex = 0;
    size_t dataSize = 0;
    size_t dataCapacity = 0;
    uint8_t* buffer = nullptr;
};
