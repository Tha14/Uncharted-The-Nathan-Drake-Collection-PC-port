// dctools/dcconv.hpp: PS3 -> PS4 Naughty Dog DC module converter (Uncharted 3) with the layout schema learned from
// the game's own PS3 / PS4 pairs; tools/dcconv/dcconv.py in C++.
//
// The PS3 module ('DC00', big endian, 32-bit pointers) and the PS4 one ('00CD', little endian, 64-bit pointers) hold
// the same object graph in the same order; only each object's layout changes (pointers widen to 8 bytes and 8-align,
// type headers [type] become [0][type], structs with pointers are 8-aligned / padded, u16 fields and byte arrays keep
// their order, u64 fields swap as a whole, script bytecode keeps its bytes but some opcodes are renumbered, source-path
// strings lose a directory prefix). The file has no type information, so layouts are learned per object key:
//   'H<type sid>'          an object with a type header (the word before it)
//   'S<symbol type>'       the value of a symbol-table entry
//   '<parent key>.<off>'   any other object, keyed by the first pointer slot that refers to it
// plus generalised keys ('<anchor>~<last steps>') for paths never seen whole. Per key the schema keeps, per PS3 4-byte
// unit, the PS4 offset and the byte transform (W 32-bit swap, H 16-bit halves swap, R raw, Q / q u64 halves, P
// pointer, T type header, D dropped), or a raw kind (str / code / blob), the start alignment and end rounding, array
// periods, per pointer-shape variants and templates for fixed-size records whose PS4 form inserts fields.
//
// Learning: every pair whose pointer graphs match is aligned (anchors from pointer slots, targets and symbols; each
// segment between pointers by an exhaustive search over word transforms and zero gaps) and the alignments vote.
#pragma once
#include "dc.hpp"

#include <functional>
#include <map>
#include <tuple>

namespace u3::dcconv {

// the number of pointer slots of a module (PS3 or PS4)
size_t slot_count(const Bytes &module);

// learn a schema from (PS3, PS4) module pairs; returns its JSON text (the format of dcconv.py learn)
std::string learn(const std::vector<std::pair<Bytes, Bytes>> &pairs, const std::function<void(const std::string &)> &log);
// Uncharted 2 (u3::g_u2): named pairs; a pair whose graphs differ, and every `partial` pair, is learned partially (the objects
// reached from the symbols both define)
std::string learn_u2(const std::vector<std::tuple<std::string, Bytes, Bytes>> &pairs, const std::vector<std::tuple<std::string, Bytes, Bytes>> &partial,
                     const std::function<void(const std::string &)> &log);

// the conversion report: a count per event, the events in the order they first occurred
struct Counts {
    std::map<std::string, int64_t> n;
    std::vector<std::string> order;
    int64_t &operator[](const std::string &k) {
        const auto [it, added] = n.try_emplace(k, 0);
        if (added) order.push_back(k);
        return it->second;
    }
};

class Converter {
public:
    // schemas (JSON texts), highest priority first: a key's whole entry comes from the first schema that has it;
    // opcode maps, string prefixes and header types are merged
    explicit Converter(const std::vector<std::string> &schemas);
    ~Converter();
    Bytes convert(const Bytes &ps3, Counts *rep = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p;
};

}  // namespace u3::dcconv
