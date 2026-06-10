#define NOMINMAX
#include "dsp/RingBuffer.h"
#include <algorithm>

namespace TelephonyDSP {

    RingBuffer::RingBuffer(size_t size) : mask(0), writePos(0), readPos(0) {
        if (size > 0) resize(size);
    }

    void RingBuffer::resize(size_t size) {
        size_t pot = 1;
        while (pot < size) pot <<= 1;
        buffer.resize(pot, 0.0f);
        mask = pot - 1;
        reset();
    }

    void RingBuffer::reset() {
        writePos = 0;
        readPos = 0;
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    }

    size_t RingBuffer::getReadAvailable() const { return writePos - readPos; }
    size_t RingBuffer::getWriteAvailable() const { return buffer.size() - (writePos - readPos); }

    size_t RingBuffer::write(const float* data, size_t count) {
        size_t available = getWriteAvailable();
        if (count > available) count = available;
        size_t off = writePos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(buffer.data() + off, data, n1 * sizeof(float));
        if (n1 < count) std::memcpy(buffer.data(), data + n1, (count - n1) * sizeof(float));
        writePos += count;
        return count;
    }

    size_t RingBuffer::read(float* data, size_t count) {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        size_t off = readPos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(data, buffer.data() + off, n1 * sizeof(float));
        if (n1 < count) std::memcpy(data + n1, buffer.data(), (count - n1) * sizeof(float));
        readPos += count;
        return count;
    }

    size_t RingBuffer::peek(float* data, size_t count) const {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        size_t off = readPos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(data, buffer.data() + off, n1 * sizeof(float));
        if (n1 < count) std::memcpy(data + n1, buffer.data(), (count - n1) * sizeof(float));
        return count;
    }

    void RingBuffer::skip(size_t count) {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        readPos += count;
    }

} // namespace TelephonyDSP
