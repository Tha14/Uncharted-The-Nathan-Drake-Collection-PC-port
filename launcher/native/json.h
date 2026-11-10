// json.h - a small JSON value for the launcher's settings file (launcher.json): parse, ordered objects, and output
// in the file's established layout (two-space indent, ", " / ": " separators, non-ASCII as \uXXXX).
#pragma once

#include <cctype>
#include <cstdlib>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Kind { Null, Bool, Int, Float, String, Array, Object } kind = Null;
    bool b = false;
    long long i = 0;
    double f = 0;
    std::string s;
    std::vector<Value> items;                                // Array
    std::vector<std::pair<std::string, Value>> fields;        // Object, in order

    static Value null() { return Value(); }
    static Value boolean(bool v) { Value x; x.kind = Bool; x.b = v; return x; }
    static Value integer(long long v) { Value x; x.kind = Int; x.i = v; return x; }
    static Value text(const std::string &v) { Value x; x.kind = String; x.s = v; return x; }
    static Value object() { Value x; x.kind = Object; return x; }

    bool is_object() const { return kind == Object; }
    const Value *get(const std::string &key) const {
        for (auto &f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    Value *get(const std::string &key) {
        for (auto &f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    // sets the key in place, or appends it (a dict's update order)
    Value &set(const std::string &key, Value v) {
        if (Value *old = get(key)) { *old = std::move(v); return *old; }
        fields.push_back({key, std::move(v)});
        return fields.back().second;
    }
    // the value already there, else the given one is added (dict.setdefault)
    Value &setdefault(const std::string &key, Value v) {
        if (Value *old = get(key)) return *old;
        fields.push_back({key, std::move(v)});
        return fields.back().second;
    }
    void remove(const std::string &key) {
        for (size_t n = 0; n < fields.size(); ++n)
            if (fields[n].first == key) { fields.erase(fields.begin() + n); return; }
    }
    // Python truthiness
    bool truthy() const {
        switch (kind) {
        case Null: return false;
        case Bool: return b;
        case Int: return i != 0;
        case Float: return f != 0;
        case String: return !s.empty();
        case Array: return !items.empty();
        case Object: return !fields.empty();
        }
        return false;
    }
    bool equals_text(const char *t) const { return kind == String && s == t; }
};

// ------------------------------------------------------------------------------------------------ parse

class Parser {
public:
    explicit Parser(const std::string &t) : t_(t) {}
    bool parse(Value &out) {
        ws();
        if (!value(out)) return false;
        ws();
        return p_ == t_.size();
    }

private:
    const std::string &t_;
    size_t p_ = 0;

    void ws() { while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_; }
    bool lit(const char *w) {
        size_t n = std::char_traits<char>::length(w);
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    static void put_utf8(std::string &o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t &v) {
        if (p_ + 4 > t_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string &o) {
        if (p_ >= t_.size() || t_[p_] != '"') return false;
        ++p_;
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p_ >= t_.size()) return false;
            char e = t_[p_++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                uint32_t c1;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && t_.compare(p_, 2, "\\u") == 0) {
                    size_t save = p_;
                    p_ += 2;
                    uint32_t c2;
                    if (hex4(c2) && c2 >= 0xDC00 && c2 < 0xE000) c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    else p_ = save;
                }
                put_utf8(o, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(Value &out) {
        size_t start = p_;
        if (p_ < t_.size() && t_[p_] == '-') ++p_;
        while (p_ < t_.size() && isdigit((unsigned char)t_[p_])) ++p_;
        bool real = false;
        if (p_ < t_.size() && t_[p_] == '.') { real = true; ++p_; while (p_ < t_.size() && isdigit((unsigned char)t_[p_])) ++p_; }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            real = true;
            ++p_;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
            while (p_ < t_.size() && isdigit((unsigned char)t_[p_])) ++p_;
        }
        std::string n = t_.substr(start, p_ - start);
        if (n.empty() || n == "-") return false;
        if (real) { out.kind = Value::Float; out.f = std::strtod(n.c_str(), nullptr); }
        else { out.kind = Value::Int; out.i = std::strtoll(n.c_str(), nullptr, 10); }
        return true;
    }
    bool value(Value &out) {
        ws();
        if (p_ >= t_.size()) return false;
        char c = t_[p_];
        if (c == '{') {
            ++p_;
            out = Value::object();
            ws();
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (p_ >= t_.size() || t_[p_++] != ':') return false;
                Value v;
                if (!value(v)) return false;
                out.set(key, std::move(v));   // a repeated key: the last value, in the first place (as Python)
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
                if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            out = Value();
            out.kind = Value::Array;
            ws();
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
            for (;;) {
                Value v;
                if (!value(v)) return false;
                out.items.push_back(std::move(v));
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
                if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (c == '"') { out = Value(); out.kind = Value::String; return string(out.s); }
        if (lit("true")) { out = Value::boolean(true); return true; }
        if (lit("false")) { out = Value::boolean(false); return true; }
        if (lit("null")) { out = Value::null(); return true; }
        if (lit("NaN")) { out = Value(); out.kind = Value::Float; out.f = NAN; return true; }
        if (lit("Infinity")) { out = Value(); out.kind = Value::Float; out.f = INFINITY; return true; }
        if (lit("-Infinity")) { out = Value(); out.kind = Value::Float; out.f = -INFINITY; return true; }
        return number(out);
    }
};

inline bool parse(const std::string &text, Value &out) {
    // a byte-order mark is not part of the value (Python's json refuses it; the file is then unreadable)
    Parser p(text);
    return p.parse(out);
}

// ------------------------------------------------------------------------------------------------ output

// ascii: Python's ensure_ascii (non-ASCII as \uXXXX); off: UTF-8 as it is (ensure_ascii=False)
inline void quote(std::string &o, const std::string &s, bool ascii = true) {
    o += '"';
    for (size_t k = 0; k < s.size();) {
        unsigned char c = (unsigned char)s[k];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && k + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[k + 1] & 0x3F); len = 2; }
            else if ((c & 0xF0) == 0xE0 && k + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[k + 1] & 0x3F) << 6) | (s[k + 2] & 0x3F); len = 3; }
            else if ((c & 0xF8) == 0xF0 && k + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[k + 1] & 0x3F) << 12) | ((s[k + 2] & 0x3F) << 6) | (s[k + 3] & 0x3F); len = 4; }
        }
        if (!ascii && cp >= 0x80) {   // ensure_ascii=False: the text's own bytes
            o.append(s, k, len);
            k += len;
            continue;
        }
        k += len;
        char buf[16];
        switch (cp) {
        case '"': o += "\\\""; continue;
        case '\\': o += "\\\\"; continue;
        case '\n': o += "\\n"; continue;
        case '\r': o += "\\r"; continue;
        case '\t': o += "\\t"; continue;
        case '\b': o += "\\b"; continue;
        case '\f': o += "\\f"; continue;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp < 0x10000)) {
            if (cp == 0x7F) { o += (char)0x7F; continue; }   // DEL stays as it is (Python's ensure_ascii)
            std::snprintf(buf, sizeof buf, "\\u%04x", cp);
            o += buf;
        } else if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000;
            std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            o += buf;
        } else {
            o += (char)cp;
        }
    }
    o += '"';
}

inline std::string real(double f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    double a = std::fabs(f);
    if (a != 0 && (a < 1e-4 || a >= 1e16)) {
        auto r = std::to_chars(buf, buf + sizeof buf, f, std::chars_format::scientific);
        std::string s(buf, r.ptr);   // "1e+16" / "1.5e-05" as Python writes them
        size_t e = s.find('e');
        std::string mant = s.substr(0, e), ex = s.substr(e + 1);
        char sign = ex[0] == '-' ? '-' : '+';
        size_t d = ex.find_first_not_of("+-0");
        std::string digits = d == std::string::npos ? "0" : ex.substr(d);
        if (digits.size() < 2) digits = "0" + digits;
        return mant + "e" + sign + digits;
    }
    auto r = std::to_chars(buf, buf + sizeof buf, f, std::chars_format::fixed);
    std::string s(buf, r.ptr);
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

inline void write(std::string &o, const Value &v, int depth, bool ascii = true) {
    std::string pad((size_t)(depth + 1) * 2, ' '), end((size_t)depth * 2, ' ');
    switch (v.kind) {
    case Value::Null: o += "null"; break;
    case Value::Bool: o += v.b ? "true" : "false"; break;
    case Value::Int: o += std::to_string(v.i); break;
    case Value::Float: o += real(v.f); break;
    case Value::String: quote(o, v.s, ascii); break;
    case Value::Array:
        if (v.items.empty()) { o += "[]"; break; }
        o += "[\n";
        for (size_t k = 0; k < v.items.size(); ++k) {
            o += pad;
            write(o, v.items[k], depth + 1, ascii);
            o += k + 1 < v.items.size() ? ",\n" : "\n";
        }
        o += end + "]";
        break;
    case Value::Object:
        if (v.fields.empty()) { o += "{}"; break; }
        o += "{\n";
        for (size_t k = 0; k < v.fields.size(); ++k) {
            o += pad;
            quote(o, v.fields[k].first, ascii);
            o += ": ";
            write(o, v.fields[k].second, depth + 1, ascii);
            o += k + 1 < v.fields.size() ? ",\n" : "\n";
        }
        o += end + "}";
        break;
    }
}

inline std::string dump(const Value &v, bool ascii = true) {
    std::string o;
    write(o, v, 0, ascii);
    return o;
}

}  // namespace json
