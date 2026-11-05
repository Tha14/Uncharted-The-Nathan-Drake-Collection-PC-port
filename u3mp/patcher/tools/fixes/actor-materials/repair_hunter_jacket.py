#!/usr/bin/env python3
"""Guarded, offline Hunter jacket repair; never overwrite inputs or active overlays.

The known donors contain constant placeholder normals/tangents. Transplanting
those is refused. --reconstruct-basis explicitly opts into a geometrically
reconstructed basis from unchanged PS4 buffers, with PS3 topology verification.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "paktools"))
import numpy as np
from ndpak import Pak, sid
from ps4writer import Blob, PakWriter
from convert_net import PS4_COOK_PREFIX
import ps3geom
import texcodec as tc

NAME = "mp-kit-hunter-torso-jacket-a"
TEMP_ROOT = Path(r"C:\Users\007\AppData\Local\Temp\kilo")
SOURCE_SHA256 = "da899e8572e1063cf86883405f64221a2a74a784bc0f536e53bbd23a59e8e701"
DONOR_SHA256 = {
    "070e2c9519c91fdf357686b9d39247bac822f66f31db7688442948ee40bfb5cf",
    "065ee28fa8e99bd8aab43d7008c98091fe27cd53166a3b6178cb9a0973aa9985",
}
PS3_SHA256 = "756cbe1838b89ae987d07f51142b2730e0e1594862dbc2c402e6c09d41bc2529"
MATERIAL = ("objects/characters/mp-kit/torso-outer/treasure-hunter/"
            "mp-kit-treasure-hunter-torso-out-jacket-a-00/default")
COLOR = ("art/objects/characters/mp-kit/textures/torso-outer/treasure-hunter/"
         "mp-kit-treasure-hunter-torso-out-jacket-a-00-color.tga/8CAC6BC4B9B5E83C.ndb")
DECAY = ("art/objects/multiplayer/loading-menu/loading-menu-cloth/texture/"
         "loading-menu-cloth-decay-color.tga/CCE61591FCB9F4F9.ndb")
FORMATS = tuple(bytes.fromhex(s) for s in ("00000c06", "01000422", "0200080a", "0300080a"))
DUPLICATE_VERTICES = {1: ((797, 326), (798, 329)), 3: ()}
BASIS_WARNING = ("Geometrically reconstructed basis: area-weighted normals and UV tangents, "
                 "not authored data or byte-exact PS3 lighting; "
                 "known constant donor vectors were NOT transplanted. No in-game validation.")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def one(values, label):
    require(len(values) == 1, f"Expected exactly one {label}, found {len(values)}")
    return values[0]


def data_at(p, ref, size):
    require(ref is not None, "Missing pointer")
    pg, off = ref
    require(0 <= pg < len(p.pages) and 0 <= off <= off + size <= p.pages[pg][1],
            f"Out-of-page data: {ref}, size {size}")
    start = p.pbase(pg) + off
    return p.d[start:start + size]


def meshes(p):
    g = one(p.items_of("GEOMETRY_1"), "geometry")
    require(p.pu32(g.page, g.data_off) == 0x10 and p.pu32(g.page, g.data_off + 8) == 4,
            "Unexpected geometry version/submesh count")
    sp, so = p.resolve(g.page, g.data_off + 0x10)
    out = []
    for i in range(4):
        off = so + i * 0x88
        nv, ni, ns, ns2 = struct.unpack("<4I", data_at(p, (sp, off + 0x20), 16))
        require(p.pu32(sp, off + 0x10) == 1 and ns2 == 0, "Unexpected submesh topology/instance streams")
        require(nv > 0 and ni > 0 and ni % 3 == 0 and 1 <= ns <= 4, "Unexpected mesh counts")
        st = p.resolve(sp, off + 0x30)
        streams = []
        for j in range(ns):
            loc = (st[0], st[1] + j * 0x18)
            attr, stride, size = struct.unpack("<HHI", data_at(p, loc, 8))
            fmt = data_at(p, p.resolve(loc[0], loc[1] + 8), 4)
            require(fmt in FORMATS and attr == 1 and stride == fmt[2] and size == nv * stride,
                    "Unexpected stream layout")
            streams.append(dict(loc=loc, fmt=fmt,
                                data=data_at(p, p.resolve(loc[0], loc[1] + 0x10), size)))
        require(len({s["fmt"] for s in streams}) == ns, "Duplicate stream semantic")
        indices = data_at(p, p.resolve(sp, off + 0x40), ni * 2)
        require(int(np.frombuffer(indices, "<u2").max()) < nv, "Invalid index/restart in triangle list")
        skin = p.resolve(sp, off + 0x60)
        skin_header = data_at(p, skin, 0x28)
        total = p.pu32(skin[0], skin[1] + 4)
        skin_data = [data_at(p, p.resolve(skin[0], skin[1] + a), n)
                     for a, n in ((8, nv * 2), (16, nv * 2), (24, total * 2), (32, total * 4))]
        out.append(dict(loc=(sp, off), nv=nv, ni=ni, streams=streams, indices=indices,
                        material=p.pstr(*p.resolve(sp, off + 0x48)),
                        skin=(skin, skin_header, skin_data)))
    return g, out


def compatible_mesh(current, donor):
    require((current["nv"], current["ni"]) == (donor["nv"], donor["ni"]), "Donor vertex/index count differs")
    require(current["material"].split(",")[0] == donor["material"].split(",")[0], "Donor material differs")
    require(current["indices"] == donor["indices"], "Donor index bytes differ")
    for i in range(2):
        a, b = current["streams"][i], donor["streams"][i]
        require(a["fmt"] == b["fmt"] == FORMATS[i] and a["data"] == b["data"],
                "Donor position/UV bytes differ")


def reconstructed_basis(position_bytes, uv_bytes, index_bytes, *, duplicate_vertices=()):
    """No welding/reindexing: area-weighted triangle normals and UV derivatives.

    Zero-area triangles contribute nothing. Enumerated zero-area-only vertices
    may copy an exact position/UV duplicate; callers must verify skin equality.
    Wholly unreferenced vertices get a canonical unit frame, never rendered.
    Other unsupported/UV-degenerate/cancelled bases fail rather than invent a
    visible surface direction. ps4actor_am.py:459,488,493 establish float16x4.
    """
    require(len(position_bytes) > 0 and len(position_bytes) % 12 == 0, "Invalid position buffer")
    nv = len(position_bytes) // 12
    require(len(uv_bytes) == nv * 4 and len(index_bytes) > 0 and len(index_bytes) % 6 == 0,
            "Invalid UV/triangle buffer")
    p = np.frombuffer(position_bytes, "<f4").reshape(-1, 3).astype(np.float64)
    uv = np.frombuffer(uv_bytes, "<f2").reshape(-1, 2).astype(np.float64)
    triangles = np.frombuffer(index_bytes, "<u2").reshape(-1, 3)
    require(np.isfinite(p).all() and np.isfinite(uv).all(), "Non-finite position/UV")
    require(int(triangles.max()) < nv, "Invalid triangle index/restart")
    referenced = np.zeros(nv, dtype=bool)
    referenced[triangles.ravel()] = True
    e1, e2 = p[triangles[:, 1]] - p[triangles[:, 0]], p[triangles[:, 2]] - p[triangles[:, 0]]
    face = np.cross(e1, e2)
    area = np.linalg.norm(face, axis=1)
    good = area > 1e-12
    d1, d2 = uv[triangles[:, 1]] - uv[triangles[:, 0]], uv[triangles[:, 2]] - uv[triangles[:, 0]]
    det = d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]
    require(np.all(np.abs(det[good]) > 1e-12), "Degenerate UV triangle")
    tangent = (e1[good] * d2[good, 1, None] - e2[good] * d1[good, 1, None]) / det[good, None]
    bitangent = (e2[good] * d1[good, 0, None] - e1[good] * d2[good, 0, None]) / det[good, None]
    n, t, b = (np.zeros((nv, 3)) for _ in range(3))
    for dest, values in ((n, face[good]), (t, tangent * area[good, None]), (b, bitangent * area[good, None])):
        np.add.at(dest, triangles[good].ravel(), np.repeat(values, 3, axis=0))
    for dest, source in duplicate_vertices:
        require(0 <= dest < nv and 0 <= source < nv and referenced[dest] and not np.any(triangles[good] == dest)
                and np.any(triangles[good] == source)
                and position_bytes[dest * 12:dest * 12 + 12] == position_bytes[source * 12:source * 12 + 12]
                and uv_bytes[dest * 4:dest * 4 + 4] == uv_bytes[source * 4:source * 4 + 4],
                "Invalid orphan-vertex duplicate")
        n[dest], t[dest], b[dest] = n[source], t[source], b[source]
    n[~referenced], t[~referenced], b[~referenced] = (0, 0, 1), (1, 0, 0), (0, 1, 0)
    for values in (n, t):
        if values is t:
            t -= n * np.sum(n * t, axis=1)[:, None]
        length = np.linalg.norm(values, axis=1)
        require(np.isfinite(length).all() and np.all(length > 1e-12),
                "Degenerate/cancelled basis or vertex without nondegenerate triangles")
        values /= length[:, None]
    handedness = np.sum(np.cross(n, t) * b, axis=1)
    require(np.isfinite(handedness).all() and np.all(np.abs(handedness) > 1e-12), "Ambiguous tangent handedness")
    normals = np.column_stack((n, np.ones(nv))).astype("<f2")
    tangents = np.column_stack((t, np.where(handedness < 0, -1, 1))).astype("<f2")
    return normals.tobytes(), tangents.tobytes(), np.flatnonzero(~good).tolist()


def build_repair(source, donor, ps3_source, *, reconstruct_basis=False):
    """Return candidate bytes/report. Hashes cannot be overridden from the CLI."""
    hashes = dict(source=sha(source), donor=sha(donor), ps3_source=sha(ps3_source))
    for key, allowed in (("source", {SOURCE_SHA256}), ("donor", DONOR_SHA256), ("ps3_source", {PS3_SHA256})):
        require(hashes[key] in allowed, f"Unexpected {key} SHA-256: {hashes[key]}")
    p, d, s = Pak(source), Pak(donor), Pak(ps3_source)
    require((p.platform, d.platform, s.platform) == ("ps4", "ps4", "ps3"), "Unexpected platforms")
    for q in (p, d, s):
        require(not q.validate(), f"Input Pak.validate failed: {q.validate()}")
    g, current = meshes(p)
    _, previous = meshes(d)
    require([m["nv"] for m in current] == [368, 805, 368, 396], "Unexpected jacket vertex counts")

    # Authenticate the exact PS3 technique slot, not a union/first-kind heuristic.
    mt = one(s.items_of("MATERIAL_TABLE_1"), "PS3 material table")
    mp, mo = mt.page, mt.data_off
    require(s.pu32(mp, mo) == 0x79 and s.pu32(mp, mo + 4) == 5 and s.pu32(mp, mo + 12) == 2,
            "Unexpected PS3 material table layout")
    ma, th, names = (s.resolve(mp, mo + off) for off in (0x20, 0x18, 0x30))
    mats = [s.resolve(ma[0], ma[1] + i * 4) for i in range(2)]
    mat = one([r for r in mats if s.pstr(*s.resolve(*r)) == MATERIAL], "PS3 visible material")
    group = s.resolve(mat[0], mat[1] + 0x10)
    node = s.resolve(group[0], group[1] + 4 * 4)
    block = s.resolve(node[0], node[1] + 0x10)
    require(s.pu32(block[0], block[1] + 4) == 26, "Unexpected PS3 technique entry count")
    for binding, index, path in ((3, 2, COLOR), (5, 4, DECAY)):
        require(s.resolve(block[0], block[1] + 0x50 + binding * 0x10) == (th[0], th[1] + 4 * index)
                and s.pu32(th[0], th[1] + index * 4) == sid(path)
                and s.pstr(*s.resolve(names[0], names[1] + index * 4)) == path,
                "PS3 technique color/decay reference differs")
    color3 = one([t for t in s.textures() if t["path"].endswith("texture1/" + COLOR)], "PS3 jacket color")
    require((color3["type"], color3["width"], color3["height"], color3["mips"], color3["depth"])
            == (1, 512, 512, 10, 1) and tc.ps3_format(color3) == ("DXT1", 8, True), "Unexpected PS3 color format")
    linear = s.vram_bytes(color3["vramOffset"], color3["vramSize"])
    require(len(linear) == tc.ps3_linear_size(512, 512, 10, 8), "Incomplete PS3 color mip chain")
    tiled = tc.ps3_to_ps4_texels(linear[128 * 128 * 8:], 256, 256, 9, 8)
    donor_color = one([t for t in d.textures() if t["path"] == PS4_COOK_PREFIX + COLOR], "donor color")
    require((donor_color["format"], donor_color["width"], donor_color["height"], donor_color["mips"])
            == (0x48, 256, 256, 9) and d.vram_bytes(donor_color["vramOffset"], donor_color["vramSize"]) == tiled,
            "Donor color differs from PS3 retained mip chain")

    basis, basis_counts = {}, {}
    protos = ps3geom.prototypes(s)
    for i, fg in ((1, NAME), (3, NAME + "-lod1")):
        c, old = current[i], previous[i]
        require([v["fmt"] for v in c["streams"]] == list(FORMATS[:2])
                and [v["fmt"] for v in old["streams"]] == list(FORMATS), "Unexpected visible streams")
        compatible_mesh(c, old)
        for stream, vector in zip(old["streams"][2:], ((0, 0, 1, 1), (1, 0, 0, 1))):
            require(stream["data"] == struct.pack("<4e", *vector) * c["nv"], "Unexpected donor basis data")
        require(reconstruct_basis, "Donor normals/tangents are constant placeholders, not a surface basis; "
                "use --reconstruct-basis only for an explicitly reconstructed diagnostic candidate")
        # ps3geom expands strip-like configs 0/1/3; ps4actor_am.py:475-480
        # emits the resulting triangles as a flat <u2 list, not a strip.
        decoded = [ps3geom.decode_block(s, pg, off, skin=False) for pg, off in protos[fg]["blocks"][1:]]
        positions = np.concatenate([b["pos"] for b in decoded]).astype("<f4").tobytes()
        uvs = np.concatenate([b["uv0"] for b in decoded]).astype("<f2").tobytes()
        triangles, base = [], 0
        for b in decoded:
            triangles.extend(tuple(v + base for v in tri) for tri in b["tris"])
            base += b["nverts"]
        require(positions == c["streams"][0]["data"] and uvs == c["streams"][1]["data"]
                and np.asarray(triangles, dtype="<u2").tobytes() == c["indices"],
                "PS3 triangle-list/position/UV provenance differs")
        counts, offsets, joints, weights = c["skin"][2]
        for dest, origin in DUPLICATE_VERTICES[i]:
            rows = []
            for vertex in (dest, origin):
                n = struct.unpack_from("<H", counts, vertex * 2)[0]
                start = struct.unpack_from("<H", offsets, vertex * 2)[0]
                rows.append((n, joints[start * 2:(start + n) * 2], weights[start * 4:(start + n) * 4]))
            require(rows[0] == rows[1], "Orphan-vertex duplicate has different skin influences")
        basis[i] = reconstructed_basis(c["streams"][0]["data"], c["streams"][1]["data"], c["indices"],
                                       duplicate_vertices=DUPLICATE_VERTICES[i])
        require(basis[i][2] == ([1191, 1192] if i == 1 else []), "Unexpected degenerate triangle set")
        ps4_triangles = np.frombuffer(c["indices"], "<u2").reshape(-1, 3)
        referenced = set(ps4_triangles.ravel())
        supported = {v for j, tri in enumerate(ps4_triangles) if j not in basis[i][2] for v in tri}
        basis_counts[str(i)] = dict(vertices=c["nv"], triangles=len(ps4_triangles),
                                    zero_area_triangles=len(basis[i][2]),
                                    zero_area_only_vertices=len(referenced - supported),
                                    unreferenced_vertices=c["nv"] - len(referenced),
                                    duplicate_basis_copies=len(DUPLICATE_VERTICES[i]))

    material_key = MATERIAL.rsplit("/", 1)[0] + ":default"
    mr = p.resolve(g.page, g.data_off + 0x50)
    require(p.pu32(g.page, g.data_off + 0x48) == 2, "Unexpected material count")
    mat = one([(mr[0], mr[1] + i * 0x48) for i in range(2)
               if p.pstr(*p.resolve(mr[0], mr[1] + i * 0x48)).split(",")[0] == material_key], "visible material")
    br = p.resolve(mat[0], mat[1] + 0x28)
    require(p.pu32(mat[0], mat[1] + 0x38) == 4, "Unexpected visible binding count")
    binding = one([(br[0], br[1] + i * 0x28) for i in range(4)
                   if p.pstr(*p.resolve(br[0], br[1] + i * 0x28)) == "g_tNdFetchColor1Map"], "Color1 binding")
    require(p.pstr(*p.resolve(binding[0], binding[1] + 0x10)) == PS4_COOK_PREFIX + DECAY[:-4], "Unexpected old Color1")
    require(sid(COLOR) not in {t["hash"] for t in p.textures()}, "Jacket color already present")

    class Writer(PakWriter):
        @property
        def vram_table_loc(self):
            return self.table.page.index, self.table.off

    w = Writer()
    w.flags = p.hdr["flags"]
    pages = [w.raw_page(p.d[off:off + size], [(o, dst) for src, dst, o in p.fixups if src == pg])
             for pg, (off, size, _) in enumerate(p.pages)]
    w.login_page = (p.hdr["loginPage"], p.hdr["loginOff"])
    extra = w.page()
    changed = [(0, 0xC, 4)]

    def clone(ref, size):
        pg, off = ref
        b = Blob().raw(data_at(p, ref, size))
        for src, dst, pos in p.fixups:
            if src == pg and off <= pos < off + size:
                require(pos + 8 <= off + size, "Partial pointer in cloned metadata")
                b.relocs.append((pos - off, pages[dst], p.pptr(pg, pos)))
        return b

    def redirect(pg, off, blob, toff=0):
        require(p.is_ptr(pg, off), "Expected existing pointer fixup")
        pages[pg].redirect(off, blob, toff)
        changed.append((pg, off, 8))

    def count(pg, off, n):
        pages[pg].set_u32(off, n)
        changed.append((pg, off, 4))

    strings = pages[0].append(Blob("jacket-strings"))
    short = strings.cstr(COLOR)
    bind_short = strings.cstr(COLOR.rsplit("/", 1)[0])
    full = strings.cstr(PS4_COOK_PREFIX + COLOR[:-4])
    vram = p.d[p.vram_base:]
    voff = (len(vram) + 255) & ~255
    w.vram = vram + b"\0" * (voff - len(vram)) + tiled
    db = Blob("jacket-color")
    db.u64(0).u64(voff).u32(len(tiled)).u32(sid(COLOR)).u32(1).u32(0x48)
    db.u32(0x010F0000).u32(9).u32(256).u32(256).u32(0).u32(3).ptr(db, 0x40)
    db.cstr(PS4_COOK_PREFIX + COLOR)
    desc = extra.item("VRAM_DESC", db)
    vt = Blob("vram-table").u32(len(p.textures()) + 1).u32(0)
    for pg, off in p.vram_desc_table():
        vt.ptr(pages[pg], off)
    vt.ptr(desc.data, -0x20)
    w.table = extra.item("VRAM_DESC_TABLE", vt, listed=False)

    tt = one(p.items_of("TEXTURE_TABLE"), "texture table")
    hp, np_, dp = (p.resolve(tt.page, tt.data_off + o) for o in (8, 16, 24))
    require(p.pu32(tt.page, tt.data_off) == len(p.textures()) == 4, "Unexpected texture table count")
    rows = []
    for i in range(4):
        nm, dr = p.resolve(np_[0], np_[1] + 8 * i), p.resolve(dp[0], dp[1] + 8 * i)
        h = p.pu32(hp[0], hp[1] + 4 * i)
        require(h == sid(p.pstr(*nm)) == p.vram_desc(*dr)["hash"], "Inconsistent texture table")
        rows.append((h, (pages[nm[0]], nm[1]), (pages[dr[0]], dr[1])))
    require([r[0] for r in rows] == sorted({r[0] for r in rows}), "Unsorted/duplicate texture hashes")
    rows.append((sid(COLOR), (strings, short), (desc.data, 0)))
    hb, nb, descs = Blob("hashes"), Blob("names"), Blob("descriptors")
    for h, nm, dr in sorted(rows, key=lambda r: r[0]):
        hb.u32(h)
        nb.ptr(*nm)
        descs.ptr(*dr)
    for field, b in ((8, hb), (16, nb), (24, descs)):
        pages[0].append(b)
        redirect(tt.page, tt.data_off + field, b)
    count(tt.page, tt.data_off, 5)

    tr = p.resolve(g.page, g.data_off + 0x40)
    require(p.pu32(g.page, g.data_off + 0x38) == 4, "Unexpected geometry texture count")
    color_row = one([i for i in range(4) if p.pstr(*p.resolve(tr[0], tr[1] + i * 0x28))
                     == PS4_COOK_PREFIX + DECAY[:-4]], "old geometry color entry")
    gt = clone(tr, 4 * 0x28)
    row = clone((tr[0], tr[1] + color_row * 0x28), 0x28)
    require([r[0] for r in row.relocs] == [0], "Unexpected geometry texture pointers")
    row.relocs = [(0, strings, full)]
    struct.pack_into("<I", row.b, 8, sid(COLOR))
    gt.embed(row, a=1)
    pages[0].append(gt)
    redirect(g.page, g.data_off + 0x40, gt)
    count(g.page, g.data_off + 0x38, 5)
    redirect(binding[0], binding[1] + 8, strings, bind_short)
    redirect(binding[0], binding[1] + 16, strings, full)

    for i, (normals, tangents, _) in basis.items():
        c = current[i]
        st = clone(c["streams"][0]["loc"], 2 * 0x18)
        for fmt, payload in zip(FORMATS[2:], (normals, tangents)):
            fb = pages[0].append(Blob().raw(fmt))
            vb = pages[0].append(Blob().raw(payload))
            st.u32((8 << 16) | 1).u32(len(payload)).ptr(fb).ptr(vb)
        pages[0].append(st)
        pg, off = c["loc"]
        count(pg, off + 0x28, 4)
        redirect(pg, off + 0x30, st)

    candidate = w.build()
    q = Pak(candidate)
    require(not q.validate() and q.vram_base == sum(q.pages[-1][:2]), "Candidate Pak.validate/VRAM boundary failed")
    require(all(size < 0x1C0000 for _, size, _ in q.pages) and len(w.vram) <= 0x80000, "Candidate exceeds existing page/VRAM budget")
    for pg, (_, size, _) in enumerate(p.pages):
        before, after = bytearray(data_at(p, (pg, 0), size)), bytearray(data_at(q, (pg, 0), size))
        for page, off, n in changed:
            if page == pg:
                before[off:off + n] = after[off:off + n] = b"\0" * n
        require(before == after and p.page_entries[pg]["entries"] == q.page_entries[pg]["entries"], "Unexpected original-page edit")
    require(set(p.fixups) <= set(q.fixups) and len(q.fixups) == len(q.ptrmap), "Lost/duplicate pointer fixup")
    require(q.d[q.vram_base:q.vram_base + len(vram)] == vram, "Original VRAM changed")
    for i, after in enumerate(meshes(q)[1]):
        require(all(after[k] == current[i][k] for k in ("nv", "ni", "indices", "material", "skin")),
                "Candidate geometry/skin changed")
        expected = [(v["fmt"], v["data"]) for v in current[i]["streams"]]
        if i in basis:
            expected += list(zip(FORMATS[2:], basis[i][:2]))
        require([(v["fmt"], v["data"]) for v in after["streams"]] == expected, "Candidate stream verification failed")
    require(q.pstr(*q.resolve(binding[0], binding[1] + 8)) == COLOR.rsplit("/", 1)[0]
            and q.pstr(*q.resolve(binding[0], binding[1] + 16)) == PS4_COOK_PREFIX + COLOR[:-4],
            "Candidate Color1 verification failed")
    return candidate, dict(input_sha256=hashes, output_sha256=sha(candidate),
                           basis_method="geometrically reconstructed basis", basis_counts=basis_counts,
                           basis_topology="little-endian uint16 triangle list; no strips/restarts",
                           basis_encoding="little-endian float16x4; normal 0200080a, tangent 0300080a",
                           basis_inputs="unchanged PS4 position/UV/index buffers; exact PS3 decoded-list match",
                           unreferenced_policy="canonical unit frame only for vertices absent from every index",
                           basis_warning=BASIS_WARNING, texture=COLOR, dimensions=[256, 256, 9],
                           skipped_zero_area_triangles={str(i): b[2] for i, b in basis.items()},
                           duplicate_vertex_basis_sources=DUPLICATE_VERTICES,
                           changed_original_page_fields=changed, original_bytes=len(source),
                           candidate_bytes=len(candidate), vram_bytes=[len(vram), len(w.vram)])


def output_path(path):
    require(TEMP_ROOT.is_dir(), "Approved temporary parent does not exist")
    path = Path(path).absolute()
    require(path.is_relative_to(TEMP_ROOT.absolute()), "Output must be under the approved temporary directory")
    require(not path.exists() and not path.is_symlink(), f"Refusing to overwrite {path}")
    path = path.resolve()
    require(path.is_relative_to(TEMP_ROOT.resolve()) and path.parent.is_dir(),
            "Output must have an existing parent under the approved temporary directory")
    require(not path.exists(), f"Refusing to overwrite {path}")
    return path


def pak_gates(source, output):
    # These existing offline checkers are required; missing imports fail closed.
    import ps4check
    import ps4verify
    import crashcheck
    old = ps4check.check(str(source), quiet=True)[0]
    new = ps4check.check(str(output), quiet=True)[0]
    require(Counter(old) == Counter(new) and all(e.startswith("unknown item type ") for e in new),
            f"ps4check regression/structural error: {new}")
    verify = ps4verify.verify(str(output), name=NAME, quiet=True)[0]
    crash = crashcheck.check(str(output))[0]
    require(not verify and not crash, f"Pak gates failed: ps4verify={verify}, crashcheck={crash}")
    return dict(Pak_validate=[], ps4check_existing_notices=new, ps4verify=verify, crashcheck=crash)


def repair(source, donor, ps3_source, output, *, reconstruct_basis=False):
    output = output_path(output)
    require(output.suffix.lower() == ".pak", "Candidate must use the .pak extension")
    paths = [Path(f).resolve(strict=True) for f in (source, donor, ps3_source)]
    require(output not in paths, "Output aliases an input")
    candidate, report = build_repair(*(f.read_bytes() for f in paths), reconstruct_basis=reconstruct_basis)
    created = False
    try:
        with output.open("xb") as f:
            created = True
            f.write(candidate)
        report["gates"] = pak_gates(paths[0], output)
    except Exception:
        if created:
            output.unlink()  # Only this newly created, rejected candidate is removed.
        raise
    report["input_paths"] = dict(zip(("source", "donor", "ps3_source"), map(str, paths)))
    report["output"] = str(output)
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "donor", "ps3-source", "output"):
        ap.add_argument("--" + name, required=True, type=Path)
    ap.add_argument("--reconstruct-basis", action="store_true", help=BASIS_WARNING)
    ap.add_argument("--report", type=Path, help="Optional new JSON file under the approved temporary directory")
    a = ap.parse_args()
    try:
        report_path = output_path(a.report) if a.report else None
        require(report_path != a.output.resolve(), "Report aliases candidate")
        result = repair(a.source, a.donor, a.ps3_source, a.output, reconstruct_basis=a.reconstruct_basis)
        text = json.dumps(result, indent=2)
        if report_path:
            with report_path.open("x", encoding="ascii") as f:
                f.write(text + "\n")
        print(text)
    except (ValueError, OSError, ImportError) as exc:
        ap.exit(2, f"Repair refused: {exc}\n")


if __name__ == "__main__":
    main()
