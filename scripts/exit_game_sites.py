"""Title menu Exit Game sites and static asset verification for CUSA57767 1.00 and 1.01.

    python scripts/exit_game_sites.py            verify boot images, title scene, layout and all 23 languages
    python scripts/exit_game_sites.py generate   write src/sos_exit_game_sites.h from the built boot images

Nothing is executed and original game files are opened read-only.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from il2cpp_index import ROOT, open_build  # noqa: E402
import prompt_assets_check as pac  # noqa: E402
from prompt_sites import fnv64  # noqa: E402

HEADER = ROOT / "src" / "sos_exit_game_sites.h"
SOURCE = ROOT / "src" / "sos_exit_game.c"
BUILDS = ("1.00", "1.01")

TOGGLE_METHOD = ("EntityComponents.PlatformSpecificToggler", "OnEnable", "Assembly-CSharp.dll")
CONFIRM_METHOD = ("Misc.ExecuteEventOnAnimationFinished", "PlayAnimation", "Assembly-CSharp.dll")
QUIT_METHOD = ("QuitApplication", "Quit", "Assembly-CSharp.dll")
TEXT_METHOD = ("UnityEngine.Localization.Components.LocalizeStringEvent", "UpdateString", "Unity.Localization.dll")

HELPERS = (
    ("UnityEngine.Component", "get_gameObject", "UnityEngine.CoreModule.dll"),
    ("UnityEngine.Component", "get_transform", "UnityEngine.CoreModule.dll"),
    ("UnityEngine.Transform", "get_parent", "UnityEngine.CoreModule.dll"),
    ("UnityEngine.Object", "get_name", "UnityEngine.CoreModule.dll"),
    ("System.String", "FastAllocateString", "mscorlib.dll"),
)

TOGGLE_ENTRY = bytes([0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53])
CONFIRM_ENTRY = bytes([0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50])
QUIT_PREFIX = bytes([0x55, 0x48, 0x89, 0xE5, 0x80, 0x3D])
QUIT_ENTRY_BYTES = 11
TEXT_ENTRY = bytes([0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53])

EXPECTED_LANGUAGES = {
    0: ("English", "en-US", "EXIT GAME"),
    3: ("Arabic", "ar", "خروج من اللعبة"),
    4: ("Portuguese (Brazil)", "pt-BR", "SAIR DO JOGO"),
    5: ("Croatian", "hr-HR", "IZLAZ IZ IGRE"),
    6: ("Czech", "cs-CZ", "UKONČIT HRU"),
    7: ("Dutch", "nl-NL", "GAME AFSLUITEN"),
    8: ("English (UK)", "en-GB", "EXIT GAME"),
    9: ("French", "fr-FR", "QUITTER LE JEU"),
    10: ("German", "de-DE", "SPIEL BEENDEN"),
    11: ("Greek", "el-GR", "ΕΞΟΔΟΣ"),
    12: ("Hungarian", "hu-HU", "KILÉPÉS A JÁTÉKBÓL"),
    13: ("Italian", "it-IT", "ESCI DAL GIOCO"),
    14: ("Japanese", "ja-JP", "ゲームを終了"),
    15: ("Korean", "ko-KR", "게임 종료"),
    16: ("Spanish (Latin America)", "es-MX", "SALIR DEL JUEGO"),
    17: ("Polish", "pl-PL", "WYJŚCIE Z GRY"),
    18: ("Portuguese (Portugal)", "pt-PT", "SAIR DO JOGO"),
    19: ("Russian", "ru-RU", "ВЫХОД ИЗ ИГРЫ"),
    20: ("Simplified Chinese", "zh-CHS", "退出游戏"),
    21: ("Spanish (Spain)", "es-ES", "SALIR DEL JUEGO"),
    22: ("Thai", "th-TH", "ออกจากเกม"),
    23: ("Traditional Chinese", "zh-CHT", "離開遊戲"),
    24: ("Turkish", "tr-TR", "OYUNDAN ÇIK"),
}

EXPECTED_VISIBLE_ROWS = [
    "ContinueButton",
    "NewGameButton",
    "LoadGameButton",
    "SettingsButton",
    "CreditsButton",
]
EXPECTED_DEV_ROWS = {"MainMenuButton_7", "MainMenuButton_6", "DevtoolButton", "DevMenuButton"}
EXPECTED_UNLOCK_ROWS = {"ThePitButton", "SpeedRunButton"}

failures = []
checks = 0


def check(ok, text):
    global checks
    checks += 1
    if not ok:
        failures.append(text)
        print("  FAIL  " + text)
    return ok


def read_bytes(index, address, size):
    return bytes(index.boot.read(address, size))


def method_range(index, spec):
    address = index.method_address(*spec)
    start, end = index.method_bounds(address)
    return start, end


def find_localization_class_slot(index):
    init_addr = index.method_address("Subsystems.LocalizationSubsystem", "Initialize")
    code = read_bytes(index, init_addr, 0x80)
    needle = bytes([0x48, 0x8B, 0x80, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x89, 0x18])
    pos = code.find(needle)
    if pos < 7 or code[pos - 7:pos - 4] != b"\x48\x8b\x05":
        raise LookupError("LocalizationSubsystem::Instance store not found")
    disp = struct.unpack_from("<i", code, pos - 4)[0]
    return init_addr + pos + disp


def analyse_build(build):
    index = open_build(build)
    if not index:
        return None
    specs = (TOGGLE_METHOD, CONFIRM_METHOD, QUIT_METHOD, TEXT_METHOD) + HELPERS
    ranges = [method_range(index, spec) for spec in specs]
    toggle_start, _ = ranges[0]
    confirm_start, _ = ranges[1]
    quit_start, _ = ranges[2]
    text_start, _ = ranges[3]

    toggle_entry = read_bytes(index, toggle_start, len(TOGGLE_ENTRY))
    confirm_entry = read_bytes(index, confirm_start, len(CONFIRM_ENTRY))
    quit_entry = read_bytes(index, quit_start, QUIT_ENTRY_BYTES)
    text_entry = read_bytes(index, text_start, len(TEXT_ENTRY))
    if toggle_entry != TOGGLE_ENTRY:
        raise ValueError(f"{build}: PlatformSpecificToggler::OnEnable prologue differs")
    if confirm_entry != CONFIRM_ENTRY:
        raise ValueError(f"{build}: ExecuteEventOnAnimationFinished::PlayAnimation prologue differs")
    if quit_entry[:len(QUIT_PREFIX)] != QUIT_PREFIX or quit_entry[10] != 0:
        raise ValueError(f"{build}: QuitApplication::Quit prologue differs")
    if text_entry != TEXT_ENTRY:
        raise ValueError(f"{build}: LocalizeStringEvent::UpdateString prologue differs")

    h = fnv64(b"".join(read_bytes(index, start, end - start) for start, end in ranges))

    loc_slot = find_localization_class_slot(index)
    lang_offset = index.field_offset("Subsystems.LocalizationSubsystem", "currentTextLanguageOption")
    enum_vals = index.enum_values("Settings.TextLanguageOptions")

    return {
        "build": build,
        "toggle_method": toggle_start,
        "confirm_method": confirm_start,
        "quit_method": quit_start,
        "text_method": text_start,
        "toggle_entry": toggle_entry,
        "confirm_entry": confirm_entry,
        "quit_entry": quit_entry,
        "text_entry": text_entry,
        "helpers": [r[0] for r in ranges[4:]],
        "ranges": ranges,
        "hash": h,
        "loc_slot": loc_slot,
        "native_object": 0x10,
        "static_fields": 0xB8,
        "text_language": lang_offset,
        "string_length": 0x10,
        "string_chars": 0x14,
        "enum_vals": enum_vals,
    }


def fmt_bytes(data):
    return ", ".join(f"0x{b:02x}" for b in data)


def render_header(results):
    lines = [
        "/* Generated by scripts/exit_game_sites.py from the built boot images. */",
        "#ifndef SOS_EXIT_GAME_SITES_H",
        "#define SOS_EXIT_GAME_SITES_H",
        "",
        '#include "sos_exit_game.h"',
        "",
    ]
    for build, info in results.items():
        tag = build.replace(".", "_")
        lines.append(f"static const SosExitGameSite kExitGame_{tag} = {{")
        lines.append(
            f"    0x{info['toggle_method']:x}u, 0x{info['confirm_method']:x}u, 0x{info['quit_method']:x}u, 0x{info['text_method']:x}u,"
        )
        lines.append(f"    {{{fmt_bytes(info['toggle_entry'])}}},")
        lines.append(f"    {{{fmt_bytes(info['confirm_entry'])}}},")
        lines.append(f"    {{{fmt_bytes(info['quit_entry'])}}},")
        lines.append(f"    {{{fmt_bytes(info['text_entry'])}}},")
        h = info["helpers"]
        lines.append(f"    0x{h[0]:x}u, 0x{h[1]:x}u, 0x{h[2]:x}u, 0x{h[3]:x}u, 0x{h[4]:x}u,")
        lines.append(f"    0x{info['loc_slot']:x}u,")
        rng = ", ".join(f"{{0x{a:x}u, 0x{b:x}u}}" for a, b in info["ranges"])
        lines.append(f"    {{{rng}}},")
        lines.append(f"    0x{info['hash']:016x}ull,")
        lines.append(
            f"    0x{info['native_object']:x}u, 0x{info['static_fields']:x}u, 0x{info['text_language']:x}u, "
            f"0x{info['string_length']:x}u, 0x{info['string_chars']:x}u,"
        )
        lines.append("};")
        lines.append("")
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def lz4_decompress(src, target_size):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out.extend(src[i:i + lit])
        i += lit
        if i >= n:
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        mlen = token & 15
        if mlen == 15:
            while True:
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(out) - offset
        for k in range(mlen):
            out.append(out[start + k])
    if len(out) != target_size:
        raise ValueError(f"LZ4 size mismatch {len(out)} != {target_size}")
    return bytes(out)


def read_unityfs(path):
    d = path.read_bytes()
    pos = 0

    def cstr():
        nonlocal pos
        end = d.index(b"\0", pos)
        s = d[pos:end].decode("utf-8", "replace")
        pos = end + 1
        return s

    if cstr() != "UnityFS":
        raise ValueError("not UnityFS")
    ver = struct.unpack_from(">I", d, pos)[0]
    pos += 4
    cstr()
    cstr()
    _, comp_info_sz, uncomp_info_sz, flags = struct.unpack_from(">QIII", d, pos)
    pos += 20
    if ver >= 7:
        pos = (pos + 15) & ~15
    info_at_end = bool(flags & 0x80)
    comp_type = flags & 0x3F
    if info_at_end:
        comp_info = d[len(d) - comp_info_sz:]
        blocks_pos = pos
    else:
        comp_info = d[pos:pos + comp_info_sz]
        blocks_pos = (pos + comp_info_sz + 15) & ~15
    info = comp_info if comp_type == 0 else lz4_decompress(comp_info, uncomp_info_sz)
    ip = 16
    bcnt = struct.unpack_from(">I", info, ip)[0]
    ip += 4
    blocks = []
    for _ in range(bcnt):
        blocks.append(struct.unpack_from(">IIH", info, ip))
        ip += 10
    ncnt = struct.unpack_from(">I", info, ip)[0]
    ip += 4
    nodes = []
    for _ in range(ncnt):
        noff, nsz, nfl = struct.unpack_from(">QQI", info, ip)
        ip += 20
        end = info.index(b"\0", ip)
        nname = info[ip:end].decode("utf-8", "replace")
        ip = end + 1
        nodes.append((nname, noff, nsz, nfl))
    raw = bytearray()
    bp = blocks_pos
    for usz, csz, bfl in blocks:
        chunk = d[bp:bp + csz]
        bp += csz
        ct = bfl & 0x3F
        raw.extend(chunk if ct == 0 else lz4_decompress(chunk, usz))
    return {nname: bytes(raw[noff:noff + nsz]) for nname, noff, nsz, _ in nodes}


def parse_serialized_bytes(data):
    r = pac.Reader(data)
    r.u32()
    r.u32()
    version = r.u32()
    data_offset = r.u32()
    endian = r.u8()
    r.pos += 3
    if version >= 22:
        r.u32()
        r.i64()
        data_offset = r.i64()
        r.i64()
    r.little = endian == 0
    r.cstr()
    r.i32()
    has_tt = r.u8() if version >= 13 else 0
    types = []
    for _ in range(r.i32()):
        cid = r.i32()
        r.u8()
        r.i16()
        if cid == 114 or cid < 0:
            r.pos += 16
        r.pos += 16
        if has_tt:
            nodes, strings = r.i32(), r.i32()
            r.pos += nodes * 32 + strings
            if version >= 21:
                dep_cnt = r.i32()
                r.pos += 4 * dep_cnt
        types.append(cid)
    objects = {}
    for _ in range(r.i32()):
        r.align()
        pid = r.i64()
        start = r.i64()
        sz = r.u32()
        tidx = r.i32()
        objects[pid] = (data_offset + start, sz, types[tidx])
    return objects


def parse_string_table(body):
    r = pac.Reader(body, 0, little=True)
    nl = r.u32()
    r.pos += nl
    r.align()
    cl = r.u32()
    locale = body[r.pos:r.pos + cl].decode("utf-8", "replace")
    r.pos += cl
    r.align()
    r.i32()
    r.i64()
    if r.i32() != 0:
        raise ValueError("unexpected table metadata")
    cnt = r.i32()
    entries = {}
    for _ in range(cnt):
        eid = r.i64()
        sl = r.u32()
        text = body[r.pos:r.pos + sl].decode("utf-8", "replace")
        r.pos += sl
        r.align()
        mc = r.i32()
        r.pos += mc * 8
        entries[eid] = text
    return locale, entries


def parse_latin_font(media):
    d, objs = pac.parse(media / "resources.assets")
    fo, fs, _ = objs[50453]
    body = bytes(d[fo + 28:fo + fs])
    pos = body.find(b"Gill Sans MT")
    r = pac.Reader(body, pos - 4, little=True)
    fl = r.u32()
    r.pos += fl
    r.align()
    sl = r.u32()
    r.pos += sl
    r.align()
    point_size = r.i32()
    r.pos += 12 + 56
    gadv = {}
    for _ in range(r.i32()):
        gi = r.u32()
        _, _, _, _, adv = struct.unpack_from("<5f", body, r.pos)
        r.pos += 48
        gadv[gi] = adv
    cadv = {}
    for _ in range(r.i32()):
        r.u8()
        r.align()
        u = r.u32()
        gi = r.u32()
        sc = struct.unpack_from("<f", body, r.pos)[0]
        r.pos += 4
        cadv[u] = gadv.get(gi, 0.0) * sc
    return point_size, cadv


def parse_greek_font(media):
    aa = media / "StreamingAssets" / "aa" / "PS4"
    files = read_unityfs(aa / "localization-assets-greek(greece)(el-gr)_assets_all.bundle")
    for fn, fd in files.items():
        if fn.endswith((".resS", ".resource")):
            continue
        objs = parse_serialized_bytes(fd)
        for pid, (o, s, c) in objs.items():
            if c == 114 and s > 5000:
                body = fd[o + 28:o + s]
                pos = body.find(b"Regular")
                if pos > 0:
                    r = pac.Reader(body, (pos + 7 + 3) & ~3, little=True)
                    ps = r.i32()
                    r.pos += 12 + 56
                    gadv = {}
                    for _ in range(r.i32()):
                        gi = r.u32()
                        _, _, _, _, adv = struct.unpack_from("<5f", body, r.pos)
                        r.pos += 48
                        gadv[gi] = adv
                    cadv = {}
                    for _ in range(r.i32()):
                        r.u8()
                        r.align()
                        u = r.u32()
                        gi = r.u32()
                        sc = struct.unpack_from("<f", body, r.pos)[0]
                        r.pos += 4
                        cadv[u] = gadv.get(gi, 0.0) * sc
                    return ps, cadv
    return 135, {}


def measure_text_width(text, latin_ps, latin_adv, greek_ps, greek_adv, font_size=24.0, char_spacing=15.0):
    cspace = char_spacing * 0.01 * font_size
    total = 0.0
    for i, ch in enumerate(text):
        u = ord(ch)
        if u in latin_adv:
            total += latin_adv[u] * (font_size / latin_ps)
        elif u in greek_adv:
            total += greek_adv[u] * (font_size / greek_ps)
        else:
            total += font_size if u >= 0x2E80 else 17.5
        if i + 1 < len(text):
            total += cspace
    return total


def verify_scene_and_layout(media):
    d3, o3 = pac.parse(media / "level3")
    gos = {}
    rt_go, children, rt_vals = {}, {}, {}
    for pid, (off, sz, cid) in o3.items():
        if cid == 1:
            r = pac.Reader(d3, off, little=True)
            cnt = r.i32()
            comps = [(r.i32(), r.i64()) for _ in range(cnt)]
            r.u32()
            nl = r.u32()
            gn = bytes(d3[r.pos:r.pos + nl]).decode("utf-8", "replace")
            r.pos += nl
            r.align()
            r.i16()
            act = r.u8()
            gos[pid] = (gn, act, comps)
        elif cid in (4, 224):
            r = pac.Reader(d3, off, little=True)
            r.i32()
            gpid = r.i64()
            r.pos += 40
            ch = []
            for _ in range(r.i32()):
                r.i32()
                ch.append(r.i64())
            r.i32()
            r.i64()
            rt_go[pid] = gpid
            children[pid] = ch
            if cid == 224 and r.pos + 40 <= off + sz:
                rt_vals[pid] = struct.unpack_from("<10f", d3, r.pos)

    mc_pid = next((pid for pid, (gn, _, _) in gos.items() if gn == "MenuChoices"), None)
    if not check(mc_pid is not None, "level3 contains MenuChoices"):
        return
    mc_tf = next(cp for _, cp in gos[mc_pid][2] if cp in rt_go)
    mc_rt = rt_vals[mc_tf]
    child_infos = []
    for ctf in children[mc_tf]:
        cgo = rt_go[ctf]
        gn, act, comps = gos[cgo]
        child_infos.append((gn, act, rt_vals[ctf], comps, children.get(ctf, [])))

    release_original = [gn for gn, act, _, _, _ in child_infos
                        if act == 1 and gn not in EXPECTED_DEV_ROWS and gn != "QuitButton"]
    check(release_original == EXPECTED_VISIBLE_ROWS,
          f"original release title menu has exact 5 rows {EXPECTED_VISIBLE_ROWS} (got {release_original})")
    check(child_infos[-1][0] == "QuitButton" and child_infos[-1][1] == 1,
          "QuitButton is the final child of MenuChoices and active in scene before PlatformSpecificToggler")

    release_extended = release_original + ["QuitButton"]
    check(len(release_extended) == 6 and release_extended[4] == "CreditsButton" and release_extended[5] == "QuitButton",
          "extended title menu has 6 rows with Credits at index 4 and QuitButton at index 5")

    # VerticalLayoutGroup spacing & 16:9 layout fit
    vlg_off, vlg_sz, _ = o3[18641]
    _, _, _, _, vlg_align, vlg_spacing = struct.unpack_from("<iiiiif", d3, vlg_off + 32)
    check(abs(vlg_spacing - 5.0) < 0.01 and vlg_align == 1,
          f"MenuChoices VerticalLayoutGroup spacing=5.0 align=UpperCenter (got {vlg_spacing}, {vlg_align})")

    credits_rt = next(rt for gn, _, rt, _, _ in child_infos if gn == "CreditsButton")
    quit_rt = child_infos[-1][2]
    row_w, row_h = quit_rt[6], quit_rt[7]
    check(abs(row_w - credits_rt[6]) < 0.01 and abs(row_h - credits_rt[7]) < 0.01,
          "QuitButton RectTransform matches CreditsButton size")
    stride = row_h + vlg_spacing
    menu_top = mc_rt[5] + mc_rt[7] * 0.5
    row_tops = [menu_top - i * stride for i in range(6)]
    row_bottoms = [t - row_h for t in row_tops]
    check(all(row_bottoms[i] - row_tops[i + 1] > 4.9 for i in range(5)),
          "no vertical overlap between consecutive rows including Credits and QuitButton")
    check(row_tops[0] <= 540.0 and row_bottoms[5] >= -540.0,
          f"all 6 rows fit within 1920x1080 canvas (top={row_tops[0]:.1f}, bottom={row_bottoms[5]:.1f})")

    # Check QuitButton highlight & text sub-objects match CreditsButton
    quit_sub = {gos[rt_go[g]][0]: rt_vals[g] for g in child_infos[-1][4]}
    credits_sub = {gos[rt_go[g]][0]: rt_vals[g] for g in next(ch for gn, _, _, _, ch in child_infos if gn == "CreditsButton")}
    for part in ("Text", "ActiveText", "Highlighter"):
        check(part in quit_sub and part in credits_sub and
              all(abs(a - b) < 0.01 for a, b in zip(quit_sub[part], credits_sub[part])),
              f"QuitButton/{part} transform matches CreditsButton/{part}")

    # Check ExplicitNavigationSetter config (FirstDepthSelectableChildren, vertical, loopOnEnds)
    ens_off, ens_sz, _ = o3[18749]
    ens_body = bytes(d3[ens_off + 32:ens_off + ens_sz])
    method = struct.unpack_from("<i", ens_body, 0)[0]
    vert, grid = ens_body[4], ens_body[5]
    loop = ens_body[16]
    chosen_cnt, excl_cnt = struct.unpack_from("<ii", ens_body, 24)
    check(method == 0 and vert == 1 and grid == 0 and loop == 1 and chosen_cnt == 0 and excl_cnt == 0,
          "ExplicitNavigationSetter uses FirstDepthSelectableChildren, vertical, wraparound, no exclusions")

    # Simulate ExplicitNavigationSetter navigation for 6 rows (with Continue) and 5 rows (without Continue)
    for label, rows in (("with Continue", release_extended), ("without Continue", release_extended[1:])):
        n = len(rows)
        down = {rows[i]: rows[(i + 1) % n] for i in range(n)}
        up = {rows[i]: rows[(i - 1) % n] for i in range(n)}
        check(down["CreditsButton"] == "QuitButton" and up["QuitButton"] == "CreditsButton" and
              down["QuitButton"] == rows[0] and up[rows[0]] == "QuitButton",
              f"navigation {label}: Credits <-> QuitButton and wraparound to {rows[0]}")


def verify_translations_and_fit(media, enum_vals):
    src_text = SOURCE.read_text(encoding="utf-8")
    # Parse kLanguages table from src/sos_exit_game.c
    table_entries = {}
    for m in re.finditer(r'\{(\d+),\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\}', src_text):
        opt = int(m.group(1))
        table_entries[opt] = (m.group(2), m.group(3), m.group(4))

    real_enum_opts = {v for k, v in enum_vals.items() if k not in ("LamsId", "LamsCheck")}
    check(set(table_entries) == real_enum_opts,
          f"sos_exit_game.c covers all {len(real_enum_opts)} real TextLanguageOptions ({len(table_entries)} found)")

    latin_ps, latin_adv = parse_latin_font(media)
    greek_ps, greek_adv = parse_greek_font(media)
    aa = media / "StreamingAssets" / "aa" / "PS4"
    tables = {}
    for p in sorted(aa.glob("localization-string-tables-*")):
        if p.stat().st_size < 10000:
            continue
        for fn, fd in read_unityfs(p).items():
            if fn.endswith((".resS", ".resource")):
                continue
            for pid, (oo, ss, cc) in parse_serialized_bytes(fd).items():
                if cc == 114 and ss > 50000:
                    loc, ents = parse_string_table(fd[oo + 28:oo + ss])
                    tables[loc] = ents

    for opt, (exp_name, exp_loc, exp_text) in sorted(EXPECTED_LANGUAGES.items()):
        got = table_entries.get(opt)
        if not check(got is not None, f"language option {opt} ({exp_loc}) present in sos_exit_game.c"):
            continue
        name, orig_9203, text = got
        check(bool(text) and text == exp_text, f"{exp_loc}: translation matches expected {exp_text!r} (got {text!r})")
        ents = tables.get(exp_loc)
        if not check(ents is not None, f"{exp_loc}: game StringTable present"):
            continue
        check(ents.get(9203) == orig_9203, f"{exp_loc}: original key 9203 matches {ents.get(9203)!r}")
        all_chars = set("".join(ents.values()))
        missing = [ch for ch in text if ch not in all_chars]
        check(not missing, f"{exp_loc}: every glyph of {text!r} exists in game localization table (missing={missing})")
        width = measure_text_width(text, latin_ps, latin_adv, greek_ps, greek_adv)
        check(width <= 385.67, f"{exp_loc}: {text!r} width {width:.1f} px fits button width 385.67 px")


def assert_safe_output(path):
    resolved = Path(path).resolve()
    for forbidden in (ROOT / "game" / "extracted", ROOT / "game" / "prepared"):
        try:
            resolved.relative_to(forbidden.resolve())
            raise PermissionError(f"refusing to write inside original game directory: {resolved}")
        except ValueError:
            pass
    return resolved


def validate_menu_model(rows, row_h, spacing, menu_top, has_quit_highlight=True, credits_action="Credits", quit_action="ExitGame"):
    """Validates menu row count, ordering, actions, highlight, navigation and 16:9 layout fit."""
    errors = []
    if len(rows) != 6:
        errors.append(f"expected 6 rows, got {len(rows)}")
    if "CreditsButton" not in rows or "QuitButton" not in rows:
        errors.append("missing CreditsButton or QuitButton")
    else:
        ci, qi = rows.index("CreditsButton"), rows.index("QuitButton")
        if ci != 4 or qi != 5:
            errors.append(f"CreditsButton must be index 4 and QuitButton index 5 (got {ci}, {qi})")
    if credits_action != "Credits":
        errors.append("Credits action shifted")
    if quit_action != "ExitGame":
        errors.append("QuitButton action wrong")
    if not has_quit_highlight:
        errors.append("QuitButton highlight/ActiveText missing")
    stride = row_h + spacing
    tops = [menu_top - i * stride for i in range(len(rows))]
    bottoms = [t - row_h for t in tops]
    if any(bottoms[i] - tops[i + 1] < 0.0 for i in range(len(rows) - 1)):
        errors.append("vertical row overlap")
    if tops and (tops[0] > 540.0 or bottoms[-1] < -540.0):
        errors.append("rows extend past 1920x1080 screen edge")
    return errors


def run_self_test():
    base_rows = ["ContinueButton", "NewGameButton", "LoadGameButton", "SettingsButton", "CreditsButton", "QuitButton"]
    ok_count = 0
    bad = []

    def expect(name, cond):
        nonlocal ok_count
        if cond:
            ok_count += 1
        else:
            bad.append(name)
            print(f"  FAIL  self-test: {name}")

    expect("valid 6-row menu passes", not validate_menu_model(base_rows, 59.4, 5.0, 105.15))
    expect("mutation: row count stays 5", bool(validate_menu_model(base_rows[:5], 59.4, 5.0, 105.15)))
    expect("mutation: Exit Game inserted before Credits",
           bool(validate_menu_model(["ContinueButton", "NewGameButton", "LoadGameButton", "SettingsButton", "QuitButton", "CreditsButton"], 59.4, 5.0, 105.15)))
    expect("mutation: Credits action shifted", bool(validate_menu_model(base_rows, 59.4, 5.0, 105.15, credits_action="ExitGame")))
    expect("mutation: Exit Game confirm calls Credits", bool(validate_menu_model(base_rows, 59.4, 5.0, 105.15, quit_action="Credits")))
    expect("mutation: layout sixth row overlaps Credits", bool(validate_menu_model(base_rows, 59.4, -10.0, 105.15)))
    expect("mutation: layout sixth row clips screen edge", bool(validate_menu_model(base_rows, 59.4, 5.0, -300.0)))
    expect("mutation: highlight sixth row omitted", bool(validate_menu_model(base_rows, 59.4, 5.0, 105.15, has_quit_highlight=False)))
    refused = False
    try:
        assert_safe_output(ROOT / "game" / "extracted" / "v1.00" / "Media" / "level3")
    except PermissionError:
        refused = True
    expect("mutation: source asset overwrite refused (extracted)", refused)
    refused_prep = False
    try:
        assert_safe_output(ROOT / "game" / "prepared" / "CUSA57767-1.00" / "Media" / "level3")
    except PermissionError:
        refused_prep = True
    expect("mutation: source asset overwrite refused (prepared)", refused_prep)
    if bad:
        print(f"Exit Game static self-test FAILED: {len(bad)} failed")
        return 1
    print(f"Exit Game static self-test passed: {ok_count} checks")
    return 0


def main(argv):
    if "--self-test" in argv:
        return run_self_test()
    results = {}
    for build in BUILDS:
        info = analyse_build(build)
        if info is None:
            print(f"[fail] CUSA57767 {build}: boot image or metadata not installed")
            return 1
        results[build] = info

    generated = render_header(results)
    if "generate" in argv:
        out = assert_safe_output(HEADER)
        out.write_text(generated, encoding="utf-8", newline="\n")
        print(f"Wrote {out.relative_to(ROOT)} for {', '.join(f'CUSA57767-{b}' for b in BUILDS)}")
        return 0

    check(HEADER.is_file() and HEADER.read_text(encoding="utf-8") == generated,
          "src/sos_exit_game_sites.h matches CUSA57767 1.00 and 1.01 boot images")
    for build, info in results.items():
        check(len(info["ranges"]) == 9, f"CUSA57767 {build}: 9 verified code ranges")
        check(info["text_language"] == 0x40, f"CUSA57767 {build}: currentTextLanguageOption at +0x40")

    media = ROOT / "game" / "prepared" / "CUSA57767-1.00" / "Media"
    verify_scene_and_layout(media)
    verify_translations_and_fit(media, results["1.00"]["enum_vals"])

    if failures:
        print(f"Exit Game static check FAILED: {len(failures)} of {checks} checks failed")
        return 1
    print(f"Exit Game static check passed: CUSA57767 1.00 and CUSA57767 1.01, 23 languages, {checks} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
