// dctools/hkconvert.cpp: see hkconvert.hpp.
#include "hkconvert.hpp"

#include "json.h"   // launcher/native/json.h
#include "u3fix.hpp"

#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <variant>

namespace u3::hk {
namespace {

struct Member {
    std::string name, type, sub;
    int64_t carr = 0, flags = 0, off = 0;
    std::optional<std::string> cls;
};
struct ClassInfo {
    std::optional<std::string> parent;
    int64_t size = 0;
    bool members_null = true;
    std::vector<Member> members;
    bool empty_members() const { return members_null || members.empty(); }
};
using DB = std::map<std::string, ClassInfo>;

std::mutex g_lock;
std::shared_ptr<DB> g_db3, g_db4;

Member mk(const char *name, const char *t, const char *sub = "VOID", int64_t off = 0, int64_t carr = 0, int64_t flags = 0, const char *cls = nullptr) {
    Member m;
    m.name = name;
    m.type = t;
    m.sub = sub;
    m.off = off;
    m.carr = carr;
    m.flags = flags;
    if (cls) m.cls = cls;
    return m;
}

DB load_db_value(const json::Value &v) {
    DB db;
    for (const auto &[c, d] : v.fields) {
        ClassInfo ci;
        if (const json::Value *x = d.get("parent"); x && x->kind == json::Value::String) ci.parent = x->s;
        if (const json::Value *x = d.get("size")) ci.size = x->i;
        if (const json::Value *x = d.get("members"); x && x->kind == json::Value::Array) {
            ci.members_null = false;
            for (const json::Value &m : x->items) {
                Member mm;
                mm.name = m.get("name")->s;
                mm.type = m.get("type")->s;
                mm.sub = m.get("sub") && m.get("sub")->kind == json::Value::String ? m.get("sub")->s : "VOID";
                mm.carr = m.get("carr") ? m.get("carr")->i : 0;
                mm.flags = m.get("flags") ? m.get("flags")->i : 0;
                mm.off = m.get("off") ? m.get("off")->i : 0;
                if (const json::Value *c2 = m.get("cls"); c2 && c2->kind == json::Value::String) mm.cls = c2->s;
                ci.members.push_back(mm);
            }
        }
        db[c] = ci;
    }
    return db;
}
DB load_db(const fs::path &p) {
    if (!fs::exists(p)) return DB();
    return load_db_value(read_json(p));
}
// Uncharted 2: PS4 class signatures read from the retail packfiles (they extend and override SIG4)
std::map<std::string, uint32_t> g_sig4_extra;
void install(std::shared_ptr<DB> db3, std::shared_ptr<DB> db4);

}  // namespace

void load_tables(const fs::path &ps3_json, const fs::path &ps4_json) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_db3) return;
    install(std::make_shared<DB>(load_db(ps3_json)), std::make_shared<DB>(load_db(ps4_json)));
}

void load_tables_u2(const std::string &json_text) {
    json::Value v;
    if (!json::parse(json_text, v)) throw Stop("hkconvert: the Uncharted 2 Havok tables are not JSON");
    std::lock_guard<std::mutex> lock(g_lock);
    g_sig4_extra.clear();
    if (const json::Value *s = v.get("sig4"))
        for (const auto &[k, x] : s->fields) g_sig4_extra[k] = uint32_t(x.i);
    install(std::make_shared<DB>(load_db_value(*v.get("db3"))), std::make_shared<DB>(load_db_value(*v.get("db4"))));
}

namespace {
void install(std::shared_ptr<DB> db3, std::shared_ptr<DB> db4) {
    DB &D3 = *db3, &D4 = *db4;
    // PS4 classes whose member arrays are run-time initialised (.bss), layouts checked against retail
    const std::map<std::string, std::vector<Member>> OVERRIDE4 = {
        {"hkpMaterial", {mk("responseType", "ENUM", "INT8", 0), mk("rollingFrictionMultiplier", "HALF", "VOID", 2), mk("friction", "REAL", "VOID", 4),
                         mk("restitution", "REAL", "VOID", 8)}},
        {"hkcdShape", {mk("type", "ENUM", "UINT8", 0x0c), mk("dispatchType", "ENUM", "UINT8", 0x0d), mk("bitsPerKey", "UINT8", "VOID", 0x0e),
                       mk("shapeInfoCodecType", "ENUM", "UINT8", 0x0f)}},
        {"hkpShapeCollection", {mk("disableWelding", "BOOL", "VOID", 0x20), mk("collectionType", "ENUM", "UINT8", 0x21)}},
        {"hkpMotion", {mk("type", "ENUM", "UINT8", 0x0c), mk("deactivationIntegrateCounter", "UINT8", "VOID", 0x0d),
                       mk("deactivationNumInactiveFrames", "UINT16", "VOID", 0x0e, 2), mk("motionState", "STRUCT", "VOID", 0x20, 0, 0, "hkMotionState"),
                       mk("inertiaAndMassInv", "VECTOR4", "VOID", 0xd0), mk("linearVelocity", "VECTOR4", "VOID", 0xe0),
                       mk("angularVelocity", "VECTOR4", "VOID", 0xf0), mk("deactivationRefPosition", "VECTOR4", "VOID", 0x100, 2),
                       mk("deactivationRefOrientation", "UINT32", "VOID", 0x120, 2),
                       mk("savedMotion", "POINTER", "STRUCT", 0x128, 0, 0, "hkpMaxSizeMotion"), mk("savedQualityTypeIndex", "UINT16", "VOID", 0x130),
                       mk("gravityFactor", "HALF", "VOID", 0x132)}},
        // 2011.3 64-bit layouts recovered from retail PS4 packfiles (member tables are run-time built)
        {"hkpConstraintInstance", {mk("owner", "POINTER", "VOID", 0x10, 0, 0x400), mk("data", "POINTER", "STRUCT", 0x18, 0, 0, "hkpConstraintData"),
                                   mk("constraintModifiers", "POINTER", "STRUCT", 0x20, 0, 0, "hkpModifierConstraintAtom"),
                                   mk("entities", "POINTER", "STRUCT", 0x28, 2, 0, "hkpEntity"), mk("priority", "ENUM", "UINT8", 0x38),
                                   mk("wantRuntime", "BOOL", "VOID", 0x39), mk("destructionRemapInfo", "ENUM", "UINT8", 0x3a),
                                   mk("name", "STRINGPTR", "VOID", 0x50), mk("userData", "ULONG", "VOID", 0x58)}},
        {"hkdBreakableShape", {mk("typeAndFlags", "UINT8", "VOID", 0x20), mk("connectivityType", "ENUM", "UINT8", 0x21),
                               mk("flexibleJointType", "ENUM", "UINT8", 0x22), mk("qualityType", "ENUM", "UINT8", 0x23),
                               mk("parent", "POINTER", "STRUCT", 0x28, 0, 0x400, "hkdBreakableShape"), mk("majorAxisSpace", "UINT32", "VOID", 0x30),
                               mk("comAndMass", "VECTOR4", "VOID", 0x40), mk("inertiaAndValues", "STRUCT", "VOID", 0x50, 0, 0, "hkHalf8"),
                               mk("children", "ARRAY", "STRUCT", 0x60, 0, 0, "hkdShapeInstanceInfo"),
                               mk("graphicsShape", "POINTER", "STRUCT", 0x70, 0, 0, "hkdGraphicsShape"),
                               mk("objectProperties", "STRUCT", "VOID", 0x78, 0, 0, "hkdObjectProperties"), mk("userData", "ULONG", "VOID", 0x88)}},
        {"hkdGraphicsShape", {mk("type", "ENUM", "UINT8", 0x0c), mk("numChildTransforms", "INT32", "VOID", 0x10)}}};
    // PS4 parents the table scan missed (constraint atoms: hkpConstraintAtom)
    for (const auto &[c, d] : D3)
        if (D4.count(c) && !D4[c].parent && d.parent && D4.count(*d.parent)) D4[c].parent = d.parent;
    // classes whose PS4 member table is built at run time but cannot have changed: the PS3 table
    static const std::set<std::string> PTRT = {"POINTER", "CSTRING", "STRINGPTR", "ULONG", "FUNCTIONPOINTER", "ARRAY", "SIMPLEARRAY", "STRUCT"};
    for (const auto &[c, d] : D3) {
        if (!D4.count(c) || !D4[c].empty_members() || d.empty_members() || D4[c].size != d.size || D4[c].parent != d.parent) continue;
        bool ptr = false;
        for (const Member &m : d.members) ptr = ptr || PTRT.count(m.type);
        if (ptr) continue;
        D4[c].members = d.members;
        D4[c].members_null = false;
    }
    const std::map<std::string, std::vector<Member>> EXTRA = {{"hkpConstraintMotor", {mk("type", "ENUM", "INT8", 0x0c)}},
                                                              {"hkpBvTreeShape", {mk("bvTreeType", "ENUM", "UINT8", 0x18)}}};
    for (const auto &[c, v] : EXTRA)
        if (D4.count(c) && D4[c].empty_members()) {
            D4[c].members = v;
            D4[c].members_null = false;
        }
    if (!D4.count("hkHalf8") && D3.count("hkHalf8")) D4["hkHalf8"] = D3["hkHalf8"];   // plain 8 x hkHalf
    for (const auto &[c, v] : OVERRIDE4)
        if (D4.count(c)) {
            D4[c].members = v;
            D4[c].members_null = false;
        }
    g_db3 = db3;
    g_db4 = db4;
}
}  // namespace

namespace {

const std::map<std::string, uint32_t> SIG4 = {
    {"hkClass", 0x75585ef6}, {"hkClassMember", 0x5c7ea4c2}, {"hkClassEnum", 0x8a3609cf}, {"hkClassEnumItem", 0xce6f8a6c},
    {"hkpRigidBody", 0xa34b3e93}, {"hkpConvexVerticesShape", 0xffafc3aa}, {"hkpConvexVerticesConnectivity", 0x63d38e9c},
    {"hkpBoxMotion", 0x78a6b020}, {"hkpSphereMotion", 0x78a6b020}, {"hkpCylinderShape", 0x2f66d4c0}, {"hkpListShape", 0x946b2f20},
    {"hkpConvexTranslateShape", 0xd697cfe7}, {"hkpBvCompressedMeshShape", 0xb63b3f27}, {"hkdBreakableBody", 0x5a0cc6fd},
    {"hkdBreakableShape", 0xae4ec162}, {"hkdBreakableShapeConnectionList", 0xe1026cd5}, {"hkdStringObject", 0x4a6c78b8},
    {"hkdPropertyFlagSet", 0xef7ca988}, {"havokGraphicsShape", 0xcb4b5fa9}, {"hkpSimpleBreakableMaterial", 0xd3e86674},
    {"hkdCompoundBreakableShape", 0xa693fb56}, {"hkdWoodController", 0x7e07d575}, {"hkpConstraintInstance", 0x34eba5f},
    {"hkpRagdollConstraintData", 0x4cc52cb3}, {"hkpSpringDamperConstraintMotor", 0x7ead26f6}, {"hkpLimitedHingeConstraintData", 0xf4eeac0a},
    {"hkpConvexTransformShape", 0xe9d81ca}, {"hkpBoxShape", 0xc1112ea}, {"hkpBreakableConstraintData", 0x7d6310c8}, {"hkpSphereShape", 0x89b169a3}};

int scalar_size(const std::string &t) {
    static const std::map<std::string, int> S = {{"BOOL", 1}, {"CHAR", 1}, {"INT8", 1}, {"UINT8", 1}, {"INT16", 2}, {"UINT16", 2},
                                                 {"HALF", 2}, {"INT32", 4}, {"UINT32", 4}, {"REAL", 4}, {"INT64", 8}, {"UINT64", 8}};
    auto it = S.find(t);
    return it == S.end() ? 0 : it->second;
}
bool scalar_signed(const std::string &t) { return t == "CHAR" || t == "INT8" || t == "INT16" || t == "INT32" || t == "INT64"; }
int vecn(const std::string &t) {
    static const std::map<std::string, int> V = {{"VECTOR4", 4}, {"QUATERNION", 4}, {"MATRIX3", 12}, {"ROTATION", 12}, {"QSTRANSFORM", 12},
                                                 {"MATRIX4", 16}, {"TRANSFORM", 16}};
    auto it = V.find(t);
    return it == V.end() ? 0 : it->second;
}

const ClassInfo &cls_of(const DB &db, const std::string &c) {
    auto it = db.find(c);
    if (it == db.end()) throw Stop("KeyError: '" + c + "'");
    return it->second;
}
std::vector<std::string> chain(const DB &db, const std::string &cls) {
    std::vector<std::string> out;
    std::optional<std::string> c = cls;
    while (c && !c->empty()) {
        out.push_back(*c);
        c = cls_of(db, *c).parent;
    }
    std::reverse(out.begin(), out.end());
    return out;
}
std::vector<Member> all_members(const DB &db, const std::string &cls) {
    std::vector<Member> ms;
    for (const std::string &c : chain(db, cls))
        for (const Member &m : cls_of(db, c).members) ms.push_back(m);
    return ms;
}
int64_t msize(const DB &db, const Member &m, int ps) {
    if (g_u2 && m.type == "VARIANT") return 2 * int64_t(ps) * std::max<int64_t>(1, m.carr);
    const std::string &t = m.type;
    int64_t s;
    if (scalar_size(t)) s = scalar_size(t);
    else if (vecn(t)) s = 4 * vecn(t);
    else if (t == "POINTER" || t == "CSTRING" || t == "STRINGPTR" || t == "ULONG" || t == "FUNCTIONPOINTER") s = ps;
    else if (t == "ENUM" || t == "FLAGS") {
        s = scalar_size(m.sub);
        if (!s) throw Stop("KeyError: '" + m.sub + "'");
    } else if (t == "STRUCT") s = cls_of(db, m.cls.value_or("")).size;
    else if (t == "ARRAY") s = ps + 8;
    else if (t == "SIMPLEARRAY") s = ps == 4 ? ps + 4 : 16;
    else if (t == "ZERO" || t == "VOID") s = 0;
    else throw Stop("KeyError: 'member type " + t + "'");
    return s * std::max<int64_t>(1, m.carr);
}
Member elem_member(const Member &m) {
    Member e;
    e.name = m.name;
    e.type = m.sub;
    e.sub = m.sub == "POINTER" ? "STRUCT" : "VOID";
    e.cls = m.cls;
    return e;
}

// object ids: a PS3 data offset, or a new object ('new', n)
struct ObjId {
    int kind = 0;
    int64_t v = 0;
    bool operator<(const ObjId &o) const { return std::tie(kind, v) < std::tie(o.kind, o.v); }
    bool operator==(const ObjId &o) const { return kind == o.kind && v == o.v; }
};

struct Val;
using Dict = std::vector<std::pair<std::string, Val>>;
struct Val {
    enum K { None, Int, Real, List, Map, Obj, Data, Str } k = None;
    __int128 i = 0;
    double r = 0;
    std::vector<Val> l;
    std::shared_ptr<Dict> d;   // shared: a Python dict is a reference
    ObjId o;
    std::string s;
    static Val integer(__int128 x) { Val v; v.k = Int; v.i = x; return v; }
    static Val real(double x) { Val v; v.k = Real; v.r = x; return v; }
};
Val *dget(Dict &d, const std::string &k) {
    for (auto &e : d)
        if (e.first == k) return &e.second;
    return nullptr;
}
void dset(Dict &d, const std::string &k, Val v) {
    if (Val *x = dget(d, k)) *x = std::move(v);
    else d.push_back({k, std::move(v)});
}

struct Packfile {
    Bytes b, D;
    bool le = false;
    int ps = 4;
    std::map<uint32_t, std::string> cn;
    std::map<uint32_t, uint32_t> loc, glob;
    std::vector<std::pair<uint32_t, std::string>> virt;
    std::map<uint32_t, std::string> objclass;
    uint32_t contents = 0;
    uint64_t ru(const Bytes &x, uint64_t o, int n) const {
        if (o + uint64_t(n) > x.size()) throw Stop("struct.error: unpack_from requires a buffer of more bytes");
        uint64_t v = 0;
        for (int k = 0; k < n; k++) v |= uint64_t(x[o + uint64_t(le ? k : n - 1 - k)]) << (8 * k);
        return v;
    }
    explicit Packfile(const Bytes &bb) : b(bb) {
        if (b.size() < 0x40) throw Stop("struct.error: packfile header");
        le = b[0x11] == 1;
        ps = b[0x10];
        const uint32_t nsec = uint32_t(ru(b, 0x14, 4));
        std::vector<std::pair<std::string, std::array<uint32_t, 7>>> secs;
        for (uint32_t i = 0; i < nsec; i++) {
            const uint64_t o = 0x40 + 0x30ull * i;
            if (o + 48 > b.size()) throw Stop("struct.error: packfile section");
            std::string nm(b.begin() + std::ptrdiff_t(o), b.begin() + std::ptrdiff_t(o + 19));
            nm = nm.substr(0, nm.find('\0'));
            std::array<uint32_t, 7> v;
            for (int k = 0; k < 7; k++) v[size_t(k)] = uint32_t(ru(b, o + 20 + 4ull * k, 4));
            secs.push_back({nm, v});
        }
        if (secs.empty()) throw Stop("IndexError: no packfile sections");
        {
            const uint32_t st = secs[0].second[0], lf = secs[0].second[1];
            const Bytes c(b.begin() + std::ptrdiff_t(std::min<uint64_t>(st, b.size())), b.begin() + std::ptrdiff_t(std::min<uint64_t>(uint64_t(st) + lf, b.size())));
            size_t i = 0;
            while (i + 5 < c.size() && !(c[i] == 0xff && c[i + 1] == 0xff && c[i + 2] == 0xff && c[i + 3] == 0xff)) {
                size_t j = i + 5;
                while (j < c.size() && c[j]) j++;
                const size_t e = j < c.size() ? j : c.size() - 1;   // bytes.find() == -1: the slice ends one before
                cn[uint32_t(i + 5)] = std::string(c.begin() + std::ptrdiff_t(i + 5), c.begin() + std::ptrdiff_t(std::max(e, i + 5)));
                if (j >= c.size()) break;
                i = j + 1;
            }
        }
        const std::array<uint32_t, 7> *ds = nullptr;
        for (const auto &s : secs)
            if (s.first == "__data__") { ds = &s.second; break; }
        if (!ds) throw Stop("IndexError: no __data__ section");
        const auto [st, lf, gf, vf, ex, im, end] = *ds;
        (void)im;
        (void)end;
        D = Bytes(b.begin() + std::ptrdiff_t(std::min<uint64_t>(st, b.size())), b.begin() + std::ptrdiff_t(std::min<uint64_t>(uint64_t(st) + lf, b.size())));
        for (uint64_t o = uint64_t(st) + lf; o < uint64_t(st) + gf; o += 8) {
            const uint32_t a = uint32_t(ru(b, o, 4)), t = uint32_t(ru(b, o + 4, 4));
            if (a != 0xffffffffu) loc[a] = t;
        }
        for (int64_t o = int64_t(st) + gf; o < int64_t(st) + int64_t(vf) - 11; o += 12) {
            const uint32_t a = uint32_t(ru(b, uint64_t(o), 4)), t = uint32_t(ru(b, uint64_t(o) + 8, 4));
            if (a != 0xffffffffu) glob[a] = t;
        }
        for (int64_t o = int64_t(st) + vf; o < int64_t(st) + int64_t(ex) - 11; o += 12) {
            const uint32_t a = uint32_t(ru(b, uint64_t(o), 4)), t = uint32_t(ru(b, uint64_t(o) + 8, 4));
            if (a == 0xffffffffu) continue;
            auto c = cn.find(t);
            if (c == cn.end()) throw Stop("KeyError: class name offset");
            virt.push_back({a, c->second});
        }
        std::sort(virt.begin(), virt.end());
        for (const auto &[a, c] : virt) objclass[a] = c;
        contents = uint32_t(ru(b, 0x1c, 4));
    }
    uint64_t u(uint64_t o, int n) const { return ru(D, o, n); }
};

Val read_struct(const Packfile &pf, const DB &db, const std::string &cls, uint64_t o);

Val read_value(const Packfile &pf, const DB &db, const Member &m, uint64_t o) {
    if (g_u2 && m.type == "VARIANT") {   // Havok 6.1 hkVariant {ptr object, ptr class} (hkRootLevelContainer): read as a pointer
        Member m2 = m;
        m2.type = "POINTER";
        return read_value(pf, db, m2, o);
    }
    if (m.carr) {
        Member m1 = m;
        m1.carr = 0;
        const int64_t s = msize(db, m1, pf.ps);
        Val v;
        v.k = Val::List;
        for (int64_t i = 0; i < m.carr; i++) v.l.push_back(read_value(pf, db, m1, o + uint64_t(s * i)));
        return v;
    }
    const std::string &t = m.type;
    auto rd_int = [&](const std::string &ty) {
        const int n = scalar_size(ty);
        uint64_t raw = pf.u(o, n);
        __int128 x = raw;
        if (scalar_signed(ty) && n < 8 && (raw >> (8 * n - 1)) & 1) x = __int128(raw) - (__int128(1) << (8 * n));
        if (scalar_signed(ty) && n == 8) x = __int128(int64_t(raw));
        return x;
    };
    auto rd_float = [&](uint64_t at) {
        const uint32_t bits = uint32_t(pf.u(at, 4));
        float f;
        std::memcpy(&f, &bits, 4);
        return double(f);
    };
    if (scalar_size(t)) {
        if (t == "REAL") return Val::real(rd_float(o));
        return Val::integer(rd_int(t));
    }
    if (vecn(t)) {
        Val v;
        v.k = Val::List;
        for (int k = 0; k < vecn(t); k++) v.l.push_back(Val::real(rd_float(o + 4ull * uint64_t(k))));
        return v;
    }
    if (t == "ENUM" || t == "FLAGS") {
        if (!scalar_size(m.sub) || m.sub == "REAL") throw Stop("KeyError: '" + m.sub + "'");
        return Val::integer(rd_int(m.sub));
    }
    if (t == "ULONG") return Val::integer(__int128(pf.u(o, pf.ps == 4 ? 4 : 8)));
    if (t == "POINTER" || t == "FUNCTIONPOINTER") {
        Val v;
        if (auto g = pf.glob.find(uint32_t(o)); g != pf.glob.end()) {
            v.k = Val::Obj;
            v.o = {0, g->second};
        } else if (auto l = pf.loc.find(uint32_t(o)); l != pf.loc.end()) {
            v.k = Val::Data;
            v.i = l->second;
        }
        return v;
    }
    if (t == "CSTRING" || t == "STRINGPTR") {
        Val v;
        if (auto l = pf.loc.find(uint32_t(o)); l != pf.loc.end()) {
            const size_t a = l->second;
            size_t e = a;
            while (e < pf.D.size() && pf.D[e]) e++;
            if (e >= pf.D.size()) e = pf.D.size() ? pf.D.size() - 1 : 0;
            v.k = Val::Str;
            if (a < e) v.s = std::string(pf.D.begin() + std::ptrdiff_t(a), pf.D.begin() + std::ptrdiff_t(e));
        }
        return v;
    }
    if (t == "STRUCT") return read_struct(pf, db, m.cls.value_or(""), o);
    if (t == "ARRAY" || t == "SIMPLEARRAY") {
        const int32_t n = int32_t(uint32_t(pf.u(o + uint64_t(pf.ps), 4)));
        Val v;
        v.k = Val::List;
        auto l = pf.loc.find(uint32_t(o));
        if (n <= 0 || l == pf.loc.end()) return v;
        const Member em = elem_member(m);
        const int64_t s = msize(db, em, pf.ps);
        for (int32_t i = 0; i < n; i++) v.l.push_back(read_value(pf, db, em, l->second + uint64_t(s * i)));
        return v;
    }
    if (t == "ZERO" || t == "VOID") return Val();
    throw Stop("KeyError: '" + t + "'");
}

Val read_struct(const Packfile &pf, const DB &db, const std::string &cls, uint64_t o) {
    Val v;
    v.k = Val::Map;
    v.d = std::make_shared<Dict>();
    for (const Member &m : all_members(db, cls)) dset(*v.d, m.name, read_value(pf, db, m, o + uint64_t(m.off)));
    return v;
}

// ------------------------------------------------------------------------------------------------ writing PS4
struct GlobEntry;
using GlobList = std::vector<GlobEntry>;
struct GlobEntry {
    bool is_list = false;
    size_t at = 0;
    ObjId target;
    std::shared_ptr<GlobList> list;
};
struct Deferred {
    bool is_str;
    size_t o;
    std::string s;
    Member m;
    Val v;
    std::shared_ptr<GlobList> mark;
};
struct Writer {
    Bytes D;
    std::vector<std::pair<uint32_t, uint32_t>> loc;
    std::shared_ptr<GlobList> glob = std::make_shared<GlobList>();
    std::vector<std::pair<size_t, std::string>> virt;
    void align(size_t a = 16) { while (D.size() % a) D.push_back(0); }
    void put(size_t at, uint64_t v, int n) {
        if (at + size_t(n) > D.size()) throw Stop("struct.error: pack_into requires a buffer of more bytes");
        for (int k = 0; k < n; k++) D[at + size_t(k)] = uint8_t(v >> (8 * k));
    }
};

uint32_t f32bits(double d) {
    const float f = float(d);
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return ((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu)) ? v | 0x400000u : v;
}
double as_double(const Val &v) {
    if (v.k == Val::Real) return v.r;
    if (v.k == Val::Int) return double(v.i);
    throw Stop("TypeError: float() argument must be a string or a real number");
}
__int128 as_int(const Val &v) {
    if (v.k == Val::Int) return v.i;
    if (v.k == Val::Real) {
        if (!std::isfinite(v.r)) throw Stop("ValueError: cannot convert float to integer");
        return __int128(std::trunc(v.r));
    }
    throw Stop("TypeError: int() argument must be a number");
}

void write_struct(Writer &w, size_t o, const std::string &cls, const Val &v, std::deque<Deferred> &deferred);

void write_value(Writer &w, size_t base, const Member &m, const Val &v, std::deque<Deferred> &deferred) {
    const DB &DB4 = *g_db4;
    const size_t o = base + size_t(m.off);
    if (m.carr) {
        Member m1 = m;
        m1.carr = 0;
        m1.off = 0;
        const int64_t s = msize(DB4, m1, 8);
        for (int64_t i = 0; i < m.carr; i++) {
            const Val none;
            const Val &x = (v.k == Val::List && size_t(i) < v.l.size()) ? v.l[size_t(i)] : none;
            write_value(w, o + size_t(s * i), m1, x, deferred);
        }
        return;
    }
    if (m.flags & 0x400) {   // NOT_SERIALIZABLE: zero (empty arrays keep DONT_DEALLOCATE, as the retail exporter wrote them)
        if (m.type == "ARRAY") w.put(o + 12, 0x80000000u, 4);
        return;
    }
    const std::string &t = m.type;
    if (v.k == Val::None) return;
    if (scalar_size(t)) {
        if (t == "REAL") w.put(o, f32bits(as_double(v)), 4);
        else {
            const int n = scalar_size(t);
            w.put(o, uint64_t(as_int(v)) & (n == 8 ? ~0ull : ((1ull << (8 * n)) - 1)), n);
        }
    } else if (vecn(t)) {
        if (v.k != Val::List || int(v.l.size()) != vecn(t)) throw Stop("struct.error: pack expected " + std::to_string(vecn(t)) + " items");
        for (int k = 0; k < vecn(t); k++) w.put(o + 4 * size_t(k), f32bits(as_double(v.l[size_t(k)])), 4);
    } else if (t == "ENUM" || t == "FLAGS") {
        const int n = scalar_size(m.sub);
        if (!n) throw Stop("KeyError: '" + m.sub + "'");
        w.put(o, uint64_t(as_int(v)) & (n == 8 ? ~0ull : ((1ull << (8 * n)) - 1)), n);
    } else if (t == "ULONG") {
        const __int128 x = as_int(v);
        if (x < 0 || x > __int128(~0ull)) throw Stop("struct.error: argument out of range");
        w.put(o, uint64_t(x), 8);
    } else if (t == "POINTER" || t == "FUNCTIONPOINTER") {
        if (v.k == Val::Obj) w.glob->push_back({false, o, v.o, nullptr});
    } else if (t == "CSTRING" || t == "STRINGPTR") {
        if (v.k != Val::Str) throw Stop("AttributeError: 'encode'");
        deferred.push_back({true, o, v.s, {}, {}, nullptr});
    } else if (t == "STRUCT")
        write_struct(w, o, m.cls.value_or(""), v, deferred);
    else if (t == "ARRAY" || t == "SIMPLEARRAY") {
        if (v.k != Val::List) throw Stop("TypeError: object has no len()");
        const size_t n = v.l.size();
        if (t == "ARRAY") {
            w.put(o, 0, 8);
            w.put(o + 8, n, 4);
            w.put(o + 12, 0x80000000u | uint32_t(n), 4);
        } else {
            w.put(o, 0, 8);
            w.put(o + 8, n, 4);
        }
        if (n) {
            auto mark = std::make_shared<GlobList>();   // the elements' global fixups go here (writer order)
            w.glob->push_back({true, 0, {}, mark});
            deferred.push_back({false, o, "", m, v, mark});
        }
    }
}

void write_struct(Writer &w, size_t o, const std::string &cls, const Val &v, std::deque<Deferred> &deferred) {
    const DB &DB4 = *g_db4;
    if (v.k != Val::Map) throw Stop("TypeError: a struct value that is not a dict");
    if (g_u2)   // members the 6.1 object lacks: arrays written empty with DONT_DEALLOCATE, as retail has them
        for (const std::string &c : chain(DB4, cls))
            for (const Member &m : cls_of(DB4, c).members)
                if (m.type == "ARRAY" && !dget(*v.d, m.name) && !m.carr && !(m.flags & 0x400)) w.put(o + size_t(m.off) + 12, 0x80000000u, 4);
    for (const std::string &c : chain(DB4, cls))
        for (const Member &m : cls_of(DB4, c).members) {
            if (const Val *x = dget(*v.d, m.name)) write_value(w, o, m, *x, deferred);
            else if (c == "hkFourTransposedPoints" && m.name == "vertices") {
                // the PS4 member <- the PS3 values (renamed by the 2010 -> 2011 class change)
                Val a;
                if (dget(*v.d, "x")) {
                    a.k = Val::List;
                    for (const char *k : {"x", "y", "z"}) {
                        const Val *e = dget(*v.d, k);
                        if (!e) throw Stop(std::string("KeyError: '") + k + "'");
                        a.l.push_back(*e);
                    }
                }
                write_value(w, o, m, a, deferred);
            }
        }
}

void flush_deferred(Writer &w, std::deque<Deferred> &deferred) {
    const DB &DB4 = *g_db4;
    while (!deferred.empty()) {
        Deferred d = deferred.front();
        deferred.pop_front();
        if (d.is_str) {
            if (!g_u2) w.align(16);   // Uncharted 2: a string follows the previous data unaligned (padded after)
            w.loc.push_back({uint32_t(d.o), uint32_t(w.D.size())});
            w.D.insert(w.D.end(), d.s.begin(), d.s.end());
            w.D.push_back(0);
            w.align(16);
        } else {
            Member em = elem_member(d.m);
            em.off = 0;
            const int64_t s = msize(DB4, em, 8);
            w.align(16);
            const size_t a = w.D.size();
            w.loc.push_back({uint32_t(d.o), uint32_t(a)});
            w.D.resize(w.D.size() + size_t(s) * d.v.l.size(), 0);
            std::deque<Deferred> sub;
            auto saved = w.glob;
            w.glob = d.mark;
            for (size_t i = 0; i < d.v.l.size(); i++) write_value(w, a + size_t(s) * i, em, d.v.l[i], sub);
            w.glob = saved;
            if (!g_u2) w.align(16);   // Uncharted 2: array data is not padded after
            deferred.insert(deferred.begin(), sub.begin(), sub.end());
        }
    }
}

void ptr_targets(const std::string &cls, const Val &v, std::vector<ObjId> &out);
void val_targets(const Member &m, const Val &x, std::vector<ObjId> &out) {
    if (x.k == Val::None) return;
    if (m.carr && x.k == Val::List) {
        Member m1 = m;
        m1.carr = 0;
        for (const Val &y : x.l) val_targets(m1, y, out);
        return;
    }
    if (m.type == "POINTER" && x.k == Val::Obj) out.push_back(x.o);
    else if (m.type == "STRUCT" && x.k == Val::Map) ptr_targets(m.cls.value_or(""), x, out);
    else if ((m.type == "ARRAY" || m.type == "SIMPLEARRAY") && x.k == Val::List) {
        const Member em = elem_member(m);
        for (const Val &y : x.l) val_targets(em, y, out);
    }
}
// pointer targets of an object value, in PS4 member order (the order the Havok writer follows)
void ptr_targets(const std::string &cls, const Val &v, std::vector<ObjId> &out) {
    const DB &DB4 = *g_db4;
    for (const std::string &c : chain(DB4, cls))
        for (const Member &m : cls_of(DB4, c).members) {
            if (m.flags & 0x400) continue;
            const Val *x = dget(*v.d, m.name);
            if (!x) continue;
            val_targets(m, *x, out);
        }
}

std::array<double, 4> quat_from_rows(const double r[3][3]) {   // 3x3 rotation (rows) -> quaternion x, y, z, w
    const double a = r[0][0], b = r[0][1], c = r[0][2], d = r[1][0], e = r[1][1], f = r[1][2], g = r[2][0], h = r[2][1], i = r[2][2];
    const double tr = a + e + i;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        return {(h - f) / s, (c - g) / s, (d - b) / s, 0.25 * s};
    }
    if (a > e && a > i) {
        const double s = std::sqrt(1.0 + a - e - i) * 2;
        return {0.25 * s, (b + d) / s, (c + g) / s, (h - f) / s};
    }
    if (e > i) {
        const double s = std::sqrt(1.0 + e - a - i) * 2;
        return {(b + d) / s, 0.25 * s, (f + h) / s, (c - g) / s};
    }
    const double s = std::sqrt(1.0 + i - a - e) * 2;
    return {(c + g) / s, (f + h) / s, 0.25 * s, (d - b) / s};
}

struct Obj {
    ObjId id;
    std::string cls;
    Val v;
};

// the 2010.2 -> 2011.3 model changes (objs may grow)
std::vector<Obj> transform_objects(std::vector<Obj> objs) {
    const DB &DB4 = *g_db4;
    int64_t nid = 0;
    std::vector<Obj> out = objs;
    for (Obj &o : objs) {
        const std::vector<std::string> ch = chain(DB4, o.cls);
        auto in_ch = [&](const char *x) { return std::find(ch.begin(), ch.end(), x) != ch.end(); };
        Dict &v = *o.v.d;
        if (in_ch("hkdBreakableShape")) {
            // 2011: the strength moved to a per-shape hkpSimpleBreakableMaterial (typeAndFlags 0x11 in all retail data)
            nid++;
            const ObjId mid{1, nid};
            Obj mo;
            mo.id = mid;
            mo.cls = "hkpSimpleBreakableMaterial";
            mo.v.k = Val::Map;
            mo.v.d = std::make_shared<Dict>();
            const Val *st = dget(v, "strength");
            dset(*mo.v.d, "strength", st ? *st : Val::real(0.0));
            dset(*mo.v.d, "typeAndFlags", Val::integer(0x11));
            out.push_back(mo);
            Val r;
            r.k = Val::Obj;
            r.o = mid;
            dset(v, "material", r);
        }
        if (in_ch("hkdController") && !dget(v, "breakingImpulse")) dset(v, "breakingImpulse", Val::real(-1.0));
        if (o.cls == "hkpConvexTransformShape")
            if (Val *t = dget(v, "transform"); t && t->k == Val::List && t->l.size() == 16) {
                // hkTransform (3 rotation columns + translation) -> QS transform (translation, quaternion, scale)
                double T[16];
                for (int k = 0; k < 16; k++) T[k] = as_double(t->l[size_t(k)]);
                double cols[3][3] = {{T[0], T[1], T[2]}, {T[4], T[5], T[6]}, {T[8], T[9], T[10]}}, sc[3];
                for (int j = 0; j < 3; j++) {
                    double sum = 0;
                    for (int k = 0; k < 3; k++) sum = sum + cols[j][k] * cols[j][k];
                    sc[j] = std::sqrt(sum);
                    if (sc[j] == 0) sc[j] = 1.0;
                    if (std::fabs(sc[j] - 1.0) < 1e-5) sc[j] = 1.0;   // retail stores an exact unit scale
                }
                double rows[3][3];
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) rows[i][j] = cols[j][i] / sc[j];
                const auto q = quat_from_rows(rows);
                Val nt;
                nt.k = Val::List;
                for (int k = 12; k < 15; k++) nt.l.push_back(t->l[size_t(k)]);
                nt.l.push_back(Val::real(0.0));
                for (double x : q) nt.l.push_back(Val::real(x));
                for (double x : sc) nt.l.push_back(Val::real(x));
                nt.l.push_back(Val::real(0.0));
                *t = nt;
                if (!dget(v, "extraScale")) {
                    Val es;
                    es.k = Val::List;
                    for (int k = 0; k < 4; k++) es.l.push_back(Val::real(0.0));
                    dset(v, "extraScale", es);
                }
            }
    }
    // the dicts are shared, so the changes are visible in out's copies too
    return out;
}

// ------------------------------------------------------------------------------------------------ Uncharted 2: Havok 6.1
// The Havok 6.1 (U2 PS3) -> 2010/2011 object model changes, run before the 2010 -> 2011 ones:
//   motions: 6.1 had STABILIZED_SPHERE (3) / STABILIZED_BOX (5) types, gone in 2011; damping values become hkHalf (the
//            upper 16 bits of the float), time and gravity factors 1.0
//   rigid bodies: callback delay and shape-key field renamed; the broad-phase quality type as retail has it
//   extended mesh shapes: subparts get typeAndFlags, the index striding enum +1 (2011 inserted INDICES_INT8), a
//            transform holding the shape scaling; cachedNumChildShapes unset
//   ragdoll / limited hinge constraints: the 2011 setupStabilization atom (disabled); ball-socket atoms the 2011 solver
//            settings; mesh subpart storage materials {filterInfo, restitution 0, friction 1.0, userData}
//   hkaBone: a 6.1 object -> an inline struct of hkaSkeleton.bones; hkaSkeletonMapperData: a 6.1 object -> the 2011
//            hkaSkeletonMapper {mapping}
uint32_t half_of(double f) {   // hkHalf: the upper 16 bits of the float
    const float x = float(f);
    uint32_t b;
    std::memcpy(&b, &x, 4);
    return b >> 16;
}
const uint32_t HALF_ONE = 0x3f80;
Val *dmap(Val *v) { return v && v->k == Val::Map ? v : nullptr; }
__int128 dint(Dict &d, const char *k, __int128 dflt) {
    const Val *x = dget(d, k);
    return x && x->k == Val::Int ? x->i : dflt;
}
void motion_61(Val *mv) {
    if (!dmap(mv)) return;
    Dict &m = *mv->d;
    static const std::map<int, int> MOTION_TYPE = {{0, 0}, {1, 1}, {2, 2}, {3, 2}, {4, 3}, {5, 3}, {6, 4}, {7, 5}, {8, 6}, {9, 7}};
    if (Val *t = dget(m, "type"); t && t->k == Val::Int && t->i >= 0 && t->i < 10) t->i = MOTION_TYPE.at(int(t->i));
    if (Val *ms = dmap(dget(m, "motionState"))) {
        for (const char *k : {"linearDamping", "angularDamping"})
            if (Val *x = dget(*ms->d, k); x && x->k == Val::Real) *x = Val::integer(half_of(x->r));
        dset(*ms->d, "timeFactor", Val::integer(HALF_ONE));
    }
    dset(m, "gravityFactor", Val::integer(HALF_ONE));
}
void subpart_61(Val *sv) {
    Dict &s = *sv->d;
    const __int128 tf = (dint(s, "type", 0) & 1) | ((dint(s, "materialIndexStridingType", 0) & 3) << 1) | (dint(s, "numMaterials", 0) << 3);
    dset(s, "typeAndFlags", Val::integer(tf));
    if (Val *st = dget(s, "stridingType"); st && st->k == Val::Int && st->i) st->i += 1;
    Val t;
    t.k = Val::List;
    for (int k = 0; k < 12; k++) t.l.push_back(Val::real(k < 7 ? 0.0 : 1.0));
    dset(s, "transform", t);
}
std::vector<Obj> transform_61(std::vector<Obj> objs) {
    static const std::set<std::string> MOTIONS = {"hkpMotion",       "hkpKeyframedRigidMotion", "hkpMaxSizeMotion",    "hkpBoxMotion",
                                                  "hkpSphereMotion", "hkpThinBoxMotion",        "hkpFixedRigidMotion", "hkpCharacterMotion"};
    static const std::map<int, int> QUALITY = {{0, 0}, {1, 1}, {2, 1}, {3, 4}, {4, 4}};
    const uint32_t stab_bits = 0x5f7ffff0;   // the retail 2011 default maxAngle
    float stab;
    std::memcpy(&stab, &stab_bits, 4);
    std::map<ObjId, const Obj *> byid;
    for (const Obj &o : objs) byid[o.id] = &o;
    std::set<ObjId> drop;
    std::vector<Obj> out;
    for (Obj &o : objs) {
        Dict &v = *o.v.d;
        if (MOTIONS.count(o.cls)) motion_61(&o.v);
        if (o.cls == "hkpRigidBody") {
            motion_61(dget(v, "motion"));
            auto pop = [&](const char *k, Val dflt) {
                for (size_t i = 0; i < v.size(); i++)
                    if (v[i].first == k) {
                        Val x = v[i].second;
                        v.erase(v.begin() + std::ptrdiff_t(i));
                        return x;
                    }
                return dflt;
            };
            dset(v, "contactPointCallbackDelay", pop("processContactCallbackDelay", Val::integer(0xffff)));
            dset(v, "numShapeKeysInContactPointProperties", pop("numUserDatasInContactPointProperties", Val::integer(0)));
            if (Val *col = dmap(dget(v, "collidable")))
                if (Val *bp = dmap(dget(*col->d, "broadPhaseHandle")))
                    if (Val *q = dget(*bp->d, "objectQualityType"); q && q->k == Val::Int) {
                        const __int128 x = q->i;
                        if (x >= 0 && x <= 4) q->i = QUALITY.at(int(x));
                        else q->i = (0 < x && x < 0x8000) ? x + 1 : x;
                    }
        }
        if (o.cls == "hkpStorageExtendedMeshShape") {
            Val sc;
            if (Val *s = dget(v, "scaling"); s && s->k == Val::List && !s->l.empty()) sc = *s;
            else {
                sc.k = Val::List;
                for (int k = 0; k < 4; k++) sc.l.push_back(Val::real(1.0));
            }
            std::vector<Val *> subs = {dget(v, "embeddedTrianglesSubpart")};
            if (Val *ts = dget(v, "trianglesSubparts"); ts && ts->k == Val::List)
                for (Val &x : ts->l) subs.push_back(&x);
            for (Val *s : subs)
                if (dmap(s)) {
                    subpart_61(s);
                    Val *t = dget(*s->d, "transform");
                    // transform[8:12] = scaling (a list slice assignment: the length follows the scaling)
                    std::vector<Val> nl(t->l.begin(), t->l.begin() + 8);
                    nl.insert(nl.end(), sc.l.begin(), sc.l.end());
                    if (t->l.size() > 12) nl.insert(nl.end(), t->l.begin() + 12, t->l.end());
                    t->l = nl;
                }
            dset(v, "cachedNumChildShapes", Val::integer(-0x80000000ll));
        }
        if ((o.cls == "hkpRagdollConstraintData" || o.cls == "hkpLimitedHingeConstraintData") && dmap(dget(v, "atoms"))) {
            Val st;
            st.k = Val::Map;
            st.d = std::make_shared<Dict>();
            dset(*st.d, "type", Val::integer(23));
            dset(*st.d, "enabled", Val::integer(0));
            dset(*st.d, "maxAngle", Val::real(double(stab)));
            dset(*dget(v, "atoms")->d, "setupStabilization", st);
        }
        if (Val *at = dmap(dget(v, "atoms")))
            if (Val *bs = dmap(dget(*at->d, "ballSocket"))) {
                dset(*bs->d, "solvingMethod", Val::integer(1));
                dset(*bs->d, "velocityStabilizationFactor", Val::integer(0x30));
                dset(*bs->d, "inertiaStabilizationFactor", Val::real(0.0));
            }
        if (o.cls == "hkpStorageExtendedMeshShapeMeshSubpartStorage") {
            Val w;
            if (Val *x = dget(v, "materials"); x && x->k == Val::List) w = *x;
            Val ms;
            ms.k = Val::List;
            for (size_t i = 0; i + 1 < w.l.size(); i += 2) {
                Val m;
                m.k = Val::Map;
                m.d = std::make_shared<Dict>();
                dset(*m.d, "filterInfo", w.l[i]);
                dset(*m.d, "restitution", Val::integer(0));
                dset(*m.d, "friction", Val::integer(HALF_ONE));
                dset(*m.d, "userData", w.l[i + 1]);
                ms.l.push_back(m);
            }
            dset(v, "materials", ms);
        }
        if (o.cls == "hkaSkeleton") {
            Val bones;
            bones.k = Val::List;
            if (Val *b = dget(v, "bones"); b && b->k == Val::List)
                for (const Val &x : b->l) {
                    if (x.k == Val::Obj && byid.count(x.o)) {
                        Val c;
                        c.k = Val::Map;
                        c.d = std::make_shared<Dict>(*byid[x.o]->v.d);   // a copy of the bone object fields
                        bones.l.push_back(c);
                        drop.insert(x.o);
                    } else if (x.k == Val::Map)
                        bones.l.push_back(x);
                }
            dset(v, "bones", bones);
        }
        if (o.cls == "hkaSkeletonMapperData") {
            Obj n;
            n.id = o.id;
            n.cls = "hkaSkeletonMapper";
            n.v.k = Val::Map;
            n.v.d = std::make_shared<Dict>();
            dset(*n.v.d, "mapping", o.v);
            out.push_back(n);
            continue;
        }
        out.push_back(o);
    }
    std::vector<Obj> kept;
    for (Obj &o : out)
        if (!drop.count(o.id)) kept.push_back(std::move(o));
    return kept;
}

void flat(const GlobList &l, std::vector<std::pair<size_t, ObjId>> &out) {
    for (const GlobEntry &e : l) {
        if (e.is_list) flat(*e.list, out);
        else out.push_back({e.at, e.target});
    }
}

void put32(Bytes &o, uint32_t v) { for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k))); }

}  // namespace

Bytes convert_packfile(const Bytes &b3) {
    if (!g_db3 || !g_db4) throw Stop("hkconvert: the reflection tables are not loaded");
    const DB &DB3 = *g_db3, &DB4 = *g_db4;
    const Packfile pf(b3);
    if (pf.ps != 4 || pf.le) throw Stop("AssertionError: expected a PS3 packfile");
    Writer w;
    std::map<ObjId, size_t> objmap;
    std::vector<Obj> objs;
    for (const auto &[a, cls] : pf.virt) {
        if (!DB3.count(cls) || !DB4.count(cls)) throw Stop("KeyError: 'class " + cls + " not in the reflection tables'");
        objs.push_back({ObjId{0, a}, cls, read_struct(pf, DB3, cls, a)});
    }
    objs = transform_objects(g_u2 ? transform_61(std::move(objs)) : std::move(objs));
    std::map<ObjId, const Obj *> byid;
    for (const Obj &o : objs) byid[o.id] = &o;   // a repeated id: the last wins
    std::vector<ObjId> order;
    std::set<ObjId> seen;
    std::function<void(const ObjId &)> visit = [&](const ObjId &oid) {
        if (seen.count(oid) || !byid.count(oid)) return;
        seen.insert(oid);
        order.push_back(oid);
        std::vector<ObjId> tg;
        ptr_targets(byid[oid]->cls, byid[oid]->v, tg);
        for (const ObjId &t : tg) visit(t);
    };
    visit(ObjId{0, pf.contents});
    for (const Obj &o : objs) visit(o.id);
    for (const ObjId &oid : order) {
        const Obj &ob = *byid[oid];
        w.align(16);
        const size_t o = w.D.size();
        objmap[oid] = o;
        w.virt.push_back({o, ob.cls});
        w.D.resize(w.D.size() + size_t(cls_of(DB4, ob.cls).size), 0);
        std::deque<Deferred> deferred;
        write_struct(w, o, ob.cls, ob.v, deferred);
        if (g_u2) w.align(16);   // Uncharted 2: an object is padded to 16 before its arrays and strings
        flush_deferred(w, deferred);
    }
    w.align(16);
    std::vector<std::pair<size_t, ObjId>> gl;
    flat(*w.glob, gl);
    std::vector<std::pair<uint32_t, uint32_t>> glob;
    for (const auto &[s, t] : gl)
        if (objmap.count(t)) glob.push_back({uint32_t(s), uint32_t(objmap[t])});
    // class names
    std::vector<std::string> classes = {"hkClass", "hkClassMember", "hkClassEnum", "hkClassEnumItem"};
    for (const auto &[o, c] : w.virt)
        if (std::find(classes.begin(), classes.end(), c) == classes.end()) classes.push_back(c);
    Bytes cn;
    std::map<std::string, uint32_t> cnoff;
    for (const std::string &c : classes) {
        auto sx = g_sig4_extra.find(c);
        auto sg = SIG4.find(c);
        if (sx == g_sig4_extra.end() && sg == SIG4.end()) throw Stop("KeyError: 'no PS4 signature for " + c + "'");
        put32(cn, sx != g_sig4_extra.end() ? sx->second : sg->second);
        cn.push_back(0x09);
        cnoff[c] = uint32_t(cn.size());
        cn.insert(cn.end(), c.begin(), c.end());
        cn.push_back(0);
    }
    while (cn.size() % 16) cn.push_back(0xff);
    if (w.virt.empty() && !pf.objclass.count(pf.contents)) throw Stop("IndexError: no objects");
    const std::string contents_cls = pf.objclass.count(pf.contents) ? pf.objclass.at(pf.contents) : w.virt[0].second;
    auto lsort = w.loc;
    if (g_u2)   // Uncharted 2: the local fixups in the order their targets were written
        std::stable_sort(lsort.begin(), lsort.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    else
        std::sort(lsort.begin(), lsort.end());
    Bytes lf, gf, vf;
    for (const auto &[s, d] : lsort) {
        put32(lf, s);
        put32(lf, d);
    }
    while (lf.size() % 16) lf.push_back(0xff);
    for (const auto &[s, d] : glob) {   // writer order, not sorted
        put32(gf, s);
        put32(gf, 2);
        put32(gf, d);
    }
    while (gf.size() % 16) gf.push_back(0xff);
    for (const auto &[o, c] : w.virt) {
        put32(vf, uint32_t(o));
        put32(vf, 0);
        put32(vf, cnoff.at(c));
    }
    while (vf.size() % 16) vf.push_back(0xff);
    static const uint8_t H0[32] = {0x57, 0xe0, 0xe0, 0x57, 0x10, 0xc0, 0xc0, 0x10, 0, 0, 0, 0, 9, 0, 0, 0, 8, 1, 1, 1, 3, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
    Bytes out(H0, H0 + 32);
    const auto co = objmap.find(ObjId{0, pf.contents});
    const uint32_t cobj = co == objmap.end() ? 0 : uint32_t(co->second);
    for (int k = 0; k < 4; k++) out[0x1c + size_t(k)] = uint8_t(cobj >> (8 * k));
    put32(out, 0);
    put32(out, cnoff.at(contents_cls));
    const char tag[] = "hk_2011.3.0-r1";
    out.insert(out.end(), tag, tag + 14);
    out.push_back(0);
    out.push_back(0xff);
    for (uint8_t c : {0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff}) out.push_back(c);
    const uint32_t st_cn = 0xd0, st_d = st_cn + uint32_t(cn.size()), dl = uint32_t(w.D.size());
    const uint32_t L = uint32_t(lf.size()), G = uint32_t(gf.size()), V = uint32_t(vf.size());
    const std::pair<const char *, std::pair<uint32_t, std::array<uint32_t, 6>>> secs[] = {
        {"__classnames__", {st_cn, {uint32_t(cn.size()), uint32_t(cn.size()), uint32_t(cn.size()), uint32_t(cn.size()), uint32_t(cn.size()), uint32_t(cn.size())}}},
        {"__types__", {st_d, {0, 0, 0, 0, 0, 0}}},
        {"__data__", {st_d, {dl, dl + L, dl + L + G, dl + L + G + V, dl + L + G + V, dl + L + G + V}}}};
    for (const auto &[tg, so] : secs) {
        Bytes t(19, 0);
        std::memcpy(t.data(), tg, std::strlen(tg));
        out.insert(out.end(), t.begin(), t.end());
        out.push_back(0xff);
        put32(out, so.first);
        for (uint32_t x : so.second) put32(out, x);
    }
    out.insert(out.end(), cn.begin(), cn.end());
    out.insert(out.end(), w.D.begin(), w.D.end());
    out.insert(out.end(), lf.begin(), lf.end());
    out.insert(out.end(), gf.begin(), gf.end());
    out.insert(out.end(), vf.begin(), vf.end());
    return out;
}

}  // namespace u3::hk
