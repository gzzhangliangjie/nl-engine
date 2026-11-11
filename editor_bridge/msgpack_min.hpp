#pragma once
// msgpack_min.hpp — 自研极简 msgpack 编解码（header-only，零依赖）
// 仅覆盖 RPC 信封所需类型：nil / bool / int / float / str / array / map。
// 不引入 vendored msgpack-c，契合本仓库轻依赖铁律；与 RPC-SPEC.md §4/§5 对齐。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <stdexcept>

namespace nl::editor::msgpack {

struct Value;
using Array = std::vector<Value>;
using Map   = std::map<std::string, Value>;

struct Value {
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Map> data;
    Value() : data(nullptr) {}
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int64_t i) : data(i) {}
    Value(int i) : data(int64_t(i)) {}
    Value(double d) : data(d) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(const std::string& s) : data(s) {}
    Value(Array a) : data(std::move(a)) {}
    Value(Map m) : data(std::move(m)) {}

    bool isNil()   const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool()  const { return std::holds_alternative<bool>(data); }
    bool isInt()   const { return std::holds_alternative<int64_t>(data); }
    bool isDouble()const { return std::holds_alternative<double>(data); }
    bool isStr()   const { return std::holds_alternative<std::string>(data); }
    bool isArray() const { return std::holds_alternative<Array>(data); }
    bool isMap()   const { return std::holds_alternative<Map>(data); }
    bool    asBool()  const { return std::get<bool>(data); }
    int64_t asInt()   const { return std::get<int64_t>(data); }
    double  asDouble()const { return std::get<double>(data); }
    const std::string& asStr()  const { return std::get<std::string>(data); }
    const Array& asArray() const { return std::get<Array>(data); }
    const Map&   asMap()   const { return std::get<Map>(data); }
    Array& asArray() { return std::get<Array>(data); }
    Map&   asMap()   { return std::get<Map>(data); }
};

class EncodeError : public std::runtime_error { using std::runtime_error::runtime_error; };
class DecodeError : public std::runtime_error { using std::runtime_error::runtime_error; };

inline void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((v >> 24) & 0xff); b.push_back((v >> 16) & 0xff);
    b.push_back((v >> 8) & 0xff);  b.push_back(v & 0xff);
}
inline void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back((v >> (i * 8)) & 0xff);
}

inline void encode(std::vector<uint8_t>& out, const Value& v);

inline void encodeStr(std::vector<uint8_t>& out, const std::string& s) {
    size_t n = s.size();
    if (n < 32) out.push_back(uint8_t(0xa0 | n));
    else if (n <= 0xff)     { out.push_back(0xd9); out.push_back((uint8_t)n); }
    else if (n <= 0xffff)   { out.push_back(0xda); out.push_back((n >> 8) & 0xff); out.push_back(n & 0xff); }
    else                    { out.push_back(0xdb); putU32(out, (uint32_t)n); }
    out.insert(out.end(), s.begin(), s.end());
}
inline void encodeArray(std::vector<uint8_t>& out, const Array& a) {
    size_t n = a.size();
    if (n < 16) out.push_back(uint8_t(0x90 | n));
    else if (n <= 0xffff) { out.push_back(0xdc); out.push_back((n >> 8) & 0xff); out.push_back(n & 0xff); }
    else                  { out.push_back(0xdd); putU32(out, (uint32_t)n); }
    for (const auto& e : a) encode(out, e);
}
inline void encodeMap(std::vector<uint8_t>& out, const Map& m) {
    size_t n = m.size();
    if (n < 16) out.push_back(uint8_t(0x80 | n));
    else if (n <= 0xffff) { out.push_back(0xde); out.push_back((n >> 8) & 0xff); out.push_back(n & 0xff); }
    else                  { out.push_back(0xdf); putU32(out, (uint32_t)n); }
    for (const auto& kv : m) {
        encodeStr(out, kv.first);
        encode(out, kv.second);
    }
}
inline void encode(std::vector<uint8_t>& out, const Value& v) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) out.push_back(0xc0);
        else if constexpr (std::is_same_v<T, bool>) out.push_back(arg ? 0xc3 : 0xc2);
        else if constexpr (std::is_same_v<T, int64_t>) {
            if (arg >= 0) {
                if (arg <= 0x7f) out.push_back((uint8_t)arg);
                else if (arg <= 0xff)        { out.push_back(0xcc); out.push_back((uint8_t)arg); }
                else if (arg <= 0xffff)      { out.push_back(0xcd); out.push_back((arg >> 8) & 0xff); out.push_back(arg & 0xff); }
                else if (arg <= 0xffffffff)   { out.push_back(0xce); putU32(out, (uint32_t)arg); }
                else                          { out.push_back(0xcf); putU64(out, (uint64_t)arg); }
            } else {
                if (arg >= -32) out.push_back((uint8_t)(0xe0 | (arg + 32)));
                else if (arg >= -128)       { out.push_back(0xd0); out.push_back((uint8_t)arg); }
                else if (arg >= -32768)     { out.push_back(0xd1); out.push_back((arg >> 8) & 0xff); out.push_back(arg & 0xff); }
                else if (arg >= -2147483648LL) { out.push_back(0xd2); putU32(out, (uint32_t)arg); }
                else                          { out.push_back(0xd3); putU64(out, (uint64_t)arg); }
            }
        }
        else if constexpr (std::is_same_v<T, double>) {
            out.push_back(0xcb);
            uint64_t u; std::memcpy(&u, &arg, 8); putU64(out, u);
        }
        else if constexpr (std::is_same_v<T, std::string>) encodeStr(out, arg);
        else if constexpr (std::is_same_v<T, Array>) encodeArray(out, arg);
        else if constexpr (std::is_same_v<T, Map>)    encodeMap(out, arg);
    }, v.data);
}
inline std::vector<uint8_t> encode(const Value& v) { std::vector<uint8_t> out; encode(out, v); return out; }

// ---- decode ----
struct Cursor { const uint8_t* p; size_t n; size_t i; };
inline uint8_t  rdU8 (Cursor& c) { if (c.i >= c.n) throw DecodeError("eof"); return c.p[c.i++]; }
inline uint16_t rdU16(Cursor& c) { uint16_t v = 0; for (int i = 0; i < 2; ++i) v = (v << 8) | rdU8(c); return v; }
inline uint32_t rdU32(Cursor& c) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | rdU8(c); return v; }
inline uint64_t rdU64(Cursor& c) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | rdU8(c); return v; }
inline double  rdF64(Cursor& c)  { uint64_t u = rdU64(c); double d; std::memcpy(&d, &u, 8); return d; }
inline float   rdF32(Cursor& c)  { uint32_t u = rdU32(c); float f; std::memcpy(&f, &u, 4); return f; }

inline Value decode(Cursor& c);
inline Value decodeStr(Cursor& c, size_t n) {
    if (c.i + n > c.n) throw DecodeError("str overflow");
    std::string s((const char*)c.p + c.i, n); c.i += n; return Value(std::move(s));
}
inline Value decodeArray(Cursor& c, size_t n) {
    Array a; a.reserve(n);
    for (size_t i = 0; i < n; ++i) a.push_back(decode(c));
    return Value(std::move(a));
}
inline Value decodeMap(Cursor& c, size_t n) {
    Map m;
    for (size_t i = 0; i < n; ++i) {
        Value k = decode(c);
        if (!k.isStr()) throw DecodeError("map key not str");
        Value v = decode(c);
        m[k.asStr()] = std::move(v);
    }
    return Value(std::move(m));
}
inline Value decode(Cursor& c) {
    uint8_t b = rdU8(c);
    if (b == 0xc0) return Value(nullptr);
    if (b == 0xc2) return Value(false);
    if (b == 0xc3) return Value(true);
    if (b <= 0x7f) return Value(int64_t(b));
    if (b >= 0xe0) return Value(int64_t(int8_t(b)));
    if (b == 0xcc) return Value(int64_t(rdU8(c)));
    if (b == 0xcd) return Value(int64_t(rdU16(c)));
    if (b == 0xce) return Value(int64_t(rdU32(c)));
    if (b == 0xcf) return Value(int64_t(rdU64(c)));
    if (b == 0xd0) return Value(int64_t(int8_t(rdU8(c))));
    if (b == 0xd1) return Value(int64_t(int16_t(rdU16(c))));
    if (b == 0xd2) return Value(int64_t(int32_t(rdU32(c))));
    if (b == 0xd3) return Value(int64_t(rdU64(c)));
    if (b == 0xca) return Value(double(rdF32(c)));
    if (b == 0xcb) return Value(rdF64(c));
    if ((b & 0xe0) == 0xa0) return decodeStr(c, b & 0x1f);
    if (b == 0xd9) return decodeStr(c, rdU8(c));
    if (b == 0xda) return decodeStr(c, rdU16(c));
    if (b == 0xdb) return decodeStr(c, rdU32(c));
    if ((b & 0xf0) == 0x90) return decodeArray(c, b & 0x0f);
    if (b == 0xdc) return decodeArray(c, rdU16(c));
    if (b == 0xdd) return decodeArray(c, rdU32(c));
    if ((b & 0xf0) == 0x80) return decodeMap(c, b & 0x0f);
    if (b == 0xde) return decodeMap(c, rdU16(c));
    if (b == 0xdf) return decodeMap(c, rdU32(c));
    throw DecodeError("unsupported msgpack type");
}
inline Value decode(const uint8_t* p, size_t n) {
    Cursor c{p, n, 0};
    Value v = decode(c);
    if (c.i != c.n) throw DecodeError("trailing bytes");
    return v;
}

} // namespace nl::editor::msgpack
