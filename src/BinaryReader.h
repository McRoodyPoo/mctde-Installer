#pragma once
//
// Minimal little-endian binary reader over an in-memory byte buffer.
// Host is assumed little-endian (x86/x64 Windows), so fixed-width reads
// are a plain memcpy. All bounds are checked; out-of-range throws.
//
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace mctde {

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}
    explicit BinaryReader(const std::vector<uint8_t>& v)
        : data_(v.data()), size_(v.size()), pos_(0) {}

    size_t position() const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return size_ - pos_; }

    void seek(size_t p) {
        if (p > size_) throw std::out_of_range("BinaryReader: seek past end");
        pos_ = p;
    }

    uint8_t  u8()  { return read<uint8_t>(); }
    uint16_t u16() { return read<uint16_t>(); }
    uint32_t u32() { return read<uint32_t>(); }
    uint64_t u64() { return read<uint64_t>(); }
    int32_t  i32() { return static_cast<int32_t>(read<uint32_t>()); }

    // Read a fixed-length ASCII string (advances position).
    std::string ascii(size_t n) {
        ensure(n);
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

    // Absolute-offset reads that do not move the cursor.
    uint32_t u32At(size_t off) const {
        if (off + sizeof(uint32_t) > size_)
            throw std::out_of_range("BinaryReader: u32At past end");
        uint32_t v;
        std::memcpy(&v, data_ + off, sizeof(v));
        return v;
    }

private:
    template <typename T>
    T read() {
        ensure(sizeof(T));
        T v;
        std::memcpy(&v, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    void ensure(size_t n) const {
        if (pos_ + n > size_)
            throw std::out_of_range("BinaryReader: read past end");
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

} // namespace mctde
