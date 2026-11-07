"""ps3_map.py - static address map of a decrypted PS3 Uncharted 2 EBOOT.elf (read-only).

Usage:
  python ps3_map.py <EBOOT.elf> imports   [--nids RPCS3_EXE] [--cache DIR]
  python ps3_map.py <EBOOT.elf> strings   <regex> [--cache DIR]
  python ps3_map.py <EBOOT.elf> xref      <hexaddr> [--cache DIR]
  python ps3_map.py <EBOOT.elf> callers   <hexaddr> [--cache DIR]
  python ps3_map.py <EBOOT.elf> callees   <hexaddr> [--cache DIR]
  python ps3_map.py <EBOOT.elf> const     <hex32>   [--cache DIR]
  python ps3_map.py <EBOOT.elf> sid       <NAME>    [--cache DIR]   (string id, CRC-32 MSB-first)
  python ps3_map.py <EBOOT.elf> netcallers [--nids RPCS3_EXE] [--cache DIR]
  python ps3_map.py <EBOOT.elf> funcs     <start-end> [--cache DIR]   (one line per function)

imports   : libraries from .lib.stub, each function's NID, name (NIDs are named by hashing
            identifiers found in --nids, e.g. rpcs3.exe, plus a built-in list), slot and stub.
strings   : strings matching a regex with every code site that computes their address and
            the function containing that site.
xref      : code sites that compute / load / access an address (kind a=address, m=memory, k=const).
callers   : direct bl call sites, plus indirect calls through a resolved OPD descriptor.
funcs     : per function: start, size, direct/indirect callers, OPD descriptors, import calls and
            the first strings it references.
netcallers: for each sceNp*/sceNpMatching2*/sys_net/cellVoice/cellNetCtl/cellHttp/cellSsl import,
            the functions that call its stub.
The analysis cache (decoded call/reference tables) goes to --cache (default <repo>/work/ps3-110).
Prints addresses, names and short strings only; no code bytes are written anywhere.
"""
import os, re, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps3_elf as P

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEF_CACHE = os.path.join(REPO, 'work', 'ps3-110')
DEF_RPCS3 = r'D:/2. PS Games/PS3/RPCS3/rpcs3.exe'

# names that do not appear verbatim in rpcs3.exe (sys_net exports use BSD names)
EXTRA_NAMES = '''accept bind connect gethostbyaddr gethostbyname getpeername getsockname getsockopt
inet_addr inet_aton inet_lnaof inet_makeaddr inet_netof inet_network inet_ntoa inet_ntop inet_pton
listen recv recvfrom recvmsg send sendmsg sendto setsockopt shutdown socket socketclose socketpoll
socketselect sys_net_initialize_network_ex sys_net_get_udpp2p_test_param sys_net_set_udpp2p_test_param
sys_net_get_lib_name_server sys_net_if_ctl sys_net_get_if_list sys_net_get_name_server
sys_net_get_netemu_test_param sys_net_get_sockinfo sys_net_get_sockinfo_ex sys_net_get_test_param
sys_net_set_lib_name_server sys_net_set_netemu_test_param sys_net_set_test_param sys_net_show_ifconfig
sys_net_show_nameserver sys_net_show_route sys_net_abort_resolver sys_net_abort_socket
sys_net_close_dump sys_net_free_thread_context sys_net_get_routing_table_af sys_net_open_dump
sys_net_read_dump sys_net_finalize_network _sys_net_errno_loc _sys_net_h_errno_loc
sys_net_set_resolver_configurations sys_net_sync_resolver_configurations sys_net_get_errno_loc
sys_net_free_thread_context sys_net_set_udpp2p_test_param'''.split()

NET_LIB = re.compile(r'^(sceNp|sys_net|cellVoice|cellNetCtl|cellHttp|cellSsl|cellRudp|cellSysutilAvc|sceNpMatching|cellGameUpdate|sceNp2)', re.I)


def opt(argv, name, default=None):
    if name in argv:
        i = argv.index(name)
        v = argv[i + 1]
        del argv[i:i + 2]
        return v
    return default


_NAMES = {}


def imports(an, nids_src=DEF_RPCS3):
    key = nids_src
    if key not in _NAMES:
        src = nids_src if nids_src and os.path.exists(nids_src) else None
        cp = os.path.join(DEF_CACHE, 'nidnames.pickle')
        import pickle
        if src and os.path.exists(cp):
            _NAMES[key] = pickle.load(open(cp, 'rb'))
        else:
            _NAMES[key] = P.nid_names_from_binary(src, EXTRA_NAMES)
            if src:
                os.makedirs(DEF_CACHE, exist_ok=True)
                pickle.dump(_NAMES[key], open(cp, 'wb'))
    return P.parse_imports(an.elf, _NAMES[key])


def sid(name):
    # CRC-32 MSB-first, poly 0x04C11DB7, init 0, no reflection, no final xor
    c = 0
    for b in name.encode():
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c


def find_strings(e, rx):
    out = set()
    r = re.compile(rx.encode())
    for va, off, fs, ms, fl in e.segs:
        seg = e.d[off:off + fs]
        for m in r.finditer(seg):
            s = m.start()
            while s > 0 and 0x20 <= seg[s - 1] < 0x7f:
                s -= 1
            end = seg.find(b'\0', m.start())
            out.add((va + s, seg[s:end].decode('latin1')))
    return sorted(out)


_REFS_SORTED = {}


def function_strings(an, f, end, n=3):
    import bisect
    if 'r' not in _REFS_SORTED:
        _REFS_SORTED['r'] = sorted((s, v) for s, v, k in an.refs if k == 'a')
        _REFS_SORTED['k'] = [x[0] for x in _REFS_SORTED['r']]
    r, keys = _REFS_SORTED['r'], _REFS_SORTED['k']
    out = []
    for j in range(bisect.bisect_left(keys, f), bisect.bisect_left(keys, end)):
        st = an.elf.cstr(r[j][1], 60)
        if st and len(st) >= 4 and st not in out:
            out.append(st.strip())
            if len(out) >= n:
                break
    return ' | '.join('"%s"' % x for x in out)


def fmt_sites(an, sites):
    return ' '.join('%x@%x%s%s' % (an.func_of(x[0]), x[0], x[1] if x[1] != 'a' else '',
                                   ('(via %x)' % x[2]) if len(x) > 2 and x[2] else '') for x in sites)


def main(argv):
    argv = list(argv)
    cache = opt(argv, '--cache', DEF_CACHE)
    nids = opt(argv, '--nids', DEF_RPCS3)
    if len(argv) < 3:
        print(__doc__)
        return 1
    an = P.load(argv[1], cache)
    cmd = argv[2]
    print('# %s sha256 %s toc %x opd %d functions %d' % (os.path.basename(argv[1]), an.sha256, an.toc,
                                                          len(an.opd), len(an.starts)))
    if cmd == 'imports':
        for lib in imports(an, nids):
            print('%s (%d)' % (lib['lib'], len(lib['funcs'])))
            for f in lib['funcs']:
                print('  %08x %-48s slot %x stub %s' % (f['nid'], f['name'], f['slot'],
                                                         '%x' % f['stub'] if f['stub'] else '-'))
    elif cmd == 'strings':
        for a, s in find_strings(an.elf, argv[3]):
            print('%08x %-70s %s' % (a, s[:70].replace('\n', '\\n'), fmt_sites(an, an.sites(a))))
    elif cmd == 'xref':
        v = int(argv[3], 16)
        for s, k in an.byval.get(v, []):
            print('%08x in %08x  %s' % (s, an.func_of(s), k))
        for o in an.opd_of.get(v, []):
            print('OPD %08x -> %08x; refs: %s' % (o, v, fmt_sites(an, an.byval.get(o, []))))
    elif cmd == 'callers':
        v = int(argv[3], 16)
        for s in sorted(an.callers.get(v, [])):
            print('%08x in %08x' % (s, an.func_of(s)))
        for o in an.opd_of.get(v, []):
            print('OPD %08x refs: %s' % (o, fmt_sites(an, an.byval.get(o, []))))
    elif cmd == 'callees':
        f = an.func_of(int(argv[3], 16))
        end = an.func_end(f)
        for s, t in an.calls:
            if f <= s < end:
                print('%08x bl %08x' % (s, t))
        for s, o in an.icalls:
            if f <= s < end:
                print('%08x bctrl OPD %x -> %x' % (s, o, an.opd.get(o, 0)))
    elif cmd in ('const', 'sid'):
        v = sid(argv[3]) if cmd == 'sid' else int(argv[3], 16)
        print('value %08x' % v)
        for s, k in an.byval.get(v, []):
            print('%08x in %08x  %s' % (s, an.func_of(s), k))
    elif cmd == 'funcs':
        a, b = [int(x, 16) for x in argv[3].split('-')]
        stubs = {}
        for lib in imports(an, nids):
            for f in lib['funcs']:
                if f['stub']:
                    stubs[f['stub']] = f['name']
        import bisect
        i = bisect.bisect_left(an.starts, a)
        refs_by_f = {}
        while i < len(an.starts) and an.starts[i] < b:
            f = an.starts[i]
            end = an.func_end(f)
            imp = sorted(set(stubs[t] for s, t in an.calls if f <= s < end and t in stubs))
            slots = [x for o in an.opd_of.get(f, []) for x in an.ptrs_to(o)]
            print('%08x %5x callers %-3d opd %-8s data %-20s %s' % (
                f, end - f, len(an.callers.get(f, [])), ','.join('%x' % o for o in an.opd_of.get(f, [])),
                ','.join('%x' % x for x in slots[:3]),
                ' '.join(imp)), function_strings(an, f, end))
            i += 1
    elif cmd == 'netcallers':
        for lib in imports(an, nids):
            for f in lib['funcs']:
                if not (NET_LIB.match(lib['lib']) or NET_LIB.match(f['name'])):
                    continue
                cs = sorted(set(an.func_of(s) for s in an.callers.get(f['stub'], []))) if f['stub'] else []
                print('%-14s %-44s stub %-7s callers %s' % (lib['lib'], f['name'],
                      '%x' % f['stub'] if f['stub'] else '-', ' '.join('%x' % c for c in cs) or '-'))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
