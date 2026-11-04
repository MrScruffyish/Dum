// Dum - tiny binary writer/reader for network messages (little endian).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct Writer {
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void f32(float v) { uint32_t t; std::memcpy(&t, &v, 4); u32(t); }
    void str(const std::string& s) {
        uint16_t n = (uint16_t)std::min<size_t>(s.size(), 65535);
        u16(n);
        buf.insert(buf.end(), s.begin(), s.begin() + n);
    }
    void bytes(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
};

struct Reader {
    const uint8_t* p;
    size_t n, pos = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    explicit Reader(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
    bool need(size_t k) { if (pos + k > n) { ok = false; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return p[pos++]; }
    uint16_t u16() { uint16_t a = u8(); uint16_t b = u8(); return (uint16_t)(a | (b << 8)); }
    uint32_t u32() { uint32_t a = u16(); uint32_t b = u16(); return a | (b << 16); }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { uint32_t t = u32(); float v; std::memcpy(&v, &t, 4); return v; }
    std::string str() {
        uint16_t k = u16();
        if (!need(k)) return {};
        std::string s((const char*)p + pos, k);
        pos += k;
        return s;
    }
    void bytes(uint8_t* out, size_t k) { if (!need(k)) return; std::memcpy(out, p + pos, k); pos += k; }
    bool atEnd() const { return pos >= n; }
};
