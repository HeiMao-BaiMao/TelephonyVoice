#pragma once

#include <vector>
#include <atomic>
#include <cstring>

namespace TelephonyDSP {

    class RingBuffer {
    public:
        RingBuffer(size_t size = 0);
        void resize(size_t size);
        void reset();
        size_t getReadAvailable() const;
        size_t getWriteAvailable() const;
        size_t write(const float* data, size_t count);
        size_t read(float* data, size_t count);
        size_t peek(float* data, size_t count) const;
        void skip(size_t count);

    private:
        std::vector<float> buffer;
        size_t mask;
        std::atomic<size_t> writePos;
        std::atomic<size_t> readPos;
    };

} // namespace TelephonyDSP
