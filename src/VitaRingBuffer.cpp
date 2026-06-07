#include "VitaRingBuffer.h"

#include <algorithm>
#include <cstdlib>

VitaRingBuffer::VitaRingBuffer(size_t dataCapacity) {
    this->dataCapacity = dataCapacity;
    buffer = static_cast<uint8_t*>(malloc(dataCapacity));
}

VitaRingBuffer::~VitaRingBuffer() {
    free(buffer);
}

size_t VitaRingBuffer::write(const uint8_t* data, size_t bytes) {
    if (bytes == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> guard(bufferMutex);
    size_t bytesToWrite = std::min(bytes, dataCapacity - dataSize);
    if (bytesToWrite <= dataCapacity - endIndex) {
        memcpy(buffer + endIndex, data, bytesToWrite);
        endIndex += bytesToWrite;
        if (endIndex == dataCapacity) {
            endIndex = 0;
        }
    } else {
        size_t firstChunkSize = dataCapacity - endIndex;
        memcpy(buffer + endIndex, data, firstChunkSize);
        size_t secondChunkSize = bytesToWrite - firstChunkSize;
        memcpy(buffer, data + firstChunkSize, secondChunkSize);
        endIndex = secondChunkSize;
    }

    dataSize += bytesToWrite;
    return bytesToWrite;
}

void VitaRingBuffer::emptyBuffer() {
    std::lock_guard<std::mutex> guard(bufferMutex);
    begIndex = 0;
    dataSize = 0;
    endIndex = 0;
}

size_t VitaRingBuffer::read(uint8_t* data, size_t bytes) {
    if (bytes == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> guard(bufferMutex);
    size_t bytesToRead = std::min(bytes, dataSize);

    if (bytesToRead <= dataCapacity - begIndex) {
        memcpy(data, buffer + begIndex, bytesToRead);
        begIndex += bytesToRead;
        if (begIndex == dataCapacity) {
            begIndex = 0;
        }
    } else {
        size_t firstChunkSize = dataCapacity - begIndex;
        memcpy(data, buffer + begIndex, firstChunkSize);
        size_t secondChunkSize = bytesToRead - firstChunkSize;
        memcpy(data + firstChunkSize, buffer, secondChunkSize);
        begIndex = secondChunkSize;
    }

    dataSize -= bytesToRead;
    return bytesToRead;
}
