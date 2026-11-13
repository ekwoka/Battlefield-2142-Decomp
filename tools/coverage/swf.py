"""SwiffPlayer.dll's inputs to the uncited classifier (uncited.py).

SwiffPlayer is three things linked together: DICE's menu host and the
`dice.*` natives the menus call (0x10001000..0x100836f0), gameswf, the Flash
player they built on (0x100a1990..0x100e0800), and libraries (GameSpy HTTP /
SOAP, Speex, expat, the CRT). The rewrite's player (src/UI) is a new
implementation of the format, not a port of gameswf, so the module needs a
few rules of its own; they are collected here.

- `natives(repo)`: every native member's body, from the constructors that
  register them (named per object in src/UI/Player/NativeTable.cpp), with
  the member's status in docs/uncited.md's natives table ("The menu
  natives"): bound / bound elsewhere -> (c), online-only / leaves the game
  -> (o), dev -> (b), unreached by the shipped movies -> (u), missing ->
  (d). MenuBindings_NativeCoverage
  (tests/MenuBindingsTest.cpp) keeps that table equal to what the rewrite
  binds, so it is the natives' equivalent of the EXE's console rows.
- `HAND`: hand-checked labels of single functions (gameswf's parts the
  shipped movies never reach, parts the rewrite replaces by design, online
  pages), each with its reason. The movies' tag and action inventory behind
  the "unused" labels is in docs/uncited.md ("SwiffPlayer.dll: gameswf and the classifier").
- the ranges and keyword lists uncited.py takes per module.
"""
import os
import re

MODULE = "SwiffPlayer.dll"

# (a): statically linked libraries.
RANGES_A = [
    (0x100836f0, 0x100a1990),  # GameSpy HTTP / SOAP, voice device enumeration, base64, Speex
    (0x100e0800, 0x100f2020),  # expat (XML_GetErrorCode..., the w3.org namespace)
    (0x100f2020, 0x100f3000),  # the CRT
]
# (o): online only, out of scope (the online account's country tables).
RANGES_O = [
    (0x100f31c0, 0x100f76c0),  # country names and codes for Profile.createOnlineAccount (online)
    (0x100f8000, 0x100fc540),  # the second copy (getCountryList's)
]
# Only the codec and parser words: the general list's 'plasma' and
# 'punkbuster' would take the menus' own server-browser natives.
LIB_KEYS = ["nb_ctl", "nb_mode", "wideband", "pitch prediction", "fixed codebook", "gamespyhttp", "soap-env",
            "w3.org/xml", "pcdata", "attlist"]
# gameswf's own debug output: the action disassembler, the timers, the
# loaders' verbose logs. 'profil' would take every native that names
# LocalProfileManager, so it is 'profiler' here.
DEV_KEYS_ADD = ["profiler", "disasm", "timers ---", ">>>>>>>>>"]
DEV_KEYS_DROP = ["profil"]

# Hand-checked labels: address -> (label, reason). 'c' present (here: the
# rewrite does it another way, by design), 'o' online only, 'u' never
# reached by the shipped movies (left out of the needed code, like (a)),
# 'b' dev, 'd' missing.
HAND = {
    # --- replaced by design (c)
    0x100b9950: ("c", "present: FrameLabel's add_frame_name (Timeline labels, src/UI/Player)"),
    0x100b9a40: ("c", "present: DefineSprite's loader (CharacterDictionary / Timeline)"),
    0x100ba270: ("c", "present: create_movie (Player::loadDfm); its .gsc cache files are replaced by design (none kept)"),
    0x100b8150: ("c", "replaced by design: the .gsc characters cache reader"),
    0x100dcc70: ("c", "replaced by design: the fontlib's cached glyph textures reader (src/UI/Characters/Font.cpp tessellates)"),
    0x100de220: ("c", "replaced by design: the fontlib's cached glyph textures writer"),
    0x100db2b0: ("c", "replaced by design: gameswf's mark-and-sweep collector (shared_ptr ownership in src/UI/Avm1)"),
    0x100c6fb0: ("c", "replaced by design: the render handler's begin_display with its timer (the rewrite's Drawer / MenuFrame)"),
    0x100c7090: ("c", "replaced by design: the render handler's end_display with its timer"),
    0x100c7120: ("c", "replaced by design: the render handler's draw_mesh_strip with its timer (MenuBatch triangles)"),
    0x100c71c0: ("c", "replaced by design: the render handler's draw_line_strip with its timer"),
    0x100c7260: ("c", "replaced by design: the render handler's draw_bitmap with its timer"),
    0x100cc530: ("c", "replaced by design: createEmptyMovieClip's _____defaultShape (a Clip's own drawing paths)"),
    0x100cc420: ("c", "present: createEmptyMovieClip (ClipMethods.cpp)"),
    0x100cff80: ("c", "present: _width's getter (DisplayObject's bounds)"),
    0x100d0020: ("c", "present: _height's getter (DisplayObject's bounds)"),
    0x100aa020: ("c", "present: as_environment::find_local (Interpreter's scope chain)"),
    0x1000e4e0: ("c", "replaced by design: the host's string conversions (UTF-8 throughout the rewrite)"),
    0x1000ec90: ("c", "replaced by design: the host's wide-string conversion"),
    # --- online only (o)
    0x1000acd0: ("o", "online: dice.hfe.ui.Swiff's HTTP request object"),
    0x1000bbb0: ("o", "online: BF2.HTTPRequest.Post"),
    0x1000bf40: ("o", "online: the drivers\\etc\\hosts check before an HTTP request"),
    0x1006db20: ("o", "online: BFHQ's stats page rows (KILLMESSAGE_*, TEXT_bestroundscore)"),
    0x1007d000: ("o", "online: BFHQ's weapon / unlock names"),
    0x1007dbf0: ("o", "online: BFHQ's game mode names"),
    0x1007ada0: ("c", "present: the stats rows' time format, #HH#:#MM# of seconds (MenuBindings::hoursMinutesFromSeconds)"),
    0x10071120: ("o", "online: BFHQ's leaderboard rows (TEXT_risingstar)"),
    0x1000c090: ("o", "online: GameSpy's stats host (stella.prod.gamespy.com)"),
    0x10063ad0: ("o", "online: the new account page's parentalAge row (selectBoxNewAcc, with the countries)"),
    0x100776e0: ("o", "online: the master server's filter text (gamever=, bf2142_ranked=1, bf2142_anticheat=1)"),
    # Propagated (c) hand-checked in coverage-9's follow-up and found online:
    # list classes the vtable-sibling vote took from the shared list base
    # (0x10060f30.., 0x10081250..). Their callees follow by callee-of.
    # bfhqComparePlayers (vtable 0x10108624; its constructor 0x1006e8e0 runs
    # 0x1006db20's BFHQ rows; per-kit / weapon / vehicle sections of
    # DAT_1012c404, which nothing offline fills; the rewrite fills no such list)
    0x1006e8e0: ("o", "online: bfhqComparePlayers' list constructor (vtable 0x10108624, 0x1006db20's rows)"),
    0x100702e0: ("o", "online: bfhqComparePlayers' list destructor"),
    0x1006d800: ("o", "online: bfhqComparePlayers' list destructor body"),
    0x1006d840: ("o", "online: bfhqComparePlayers' list, slot 4"),
    0x1006d490: ("o", "online: bfhqComparePlayers' row fill (sections of DAT_1012c404; 0x1006a960 / 0x1006acd0 / "
                      "0x1006b610 / 0x1006cbe0 / 0x1006d160 per section)"),
    0x1006a830: ("o", "online: bfhqComparePlayers' list, slot 13"),
    # the buddy list (vtable 0x10107c60: "buddy", TEXT_buddyrequest, TEXT_online / TEXT_offline)
    0x10061ff0: ("o", "online: the buddy list's constructor (ProfileManager's buddies)"),
    0x10062160: ("o", "online: the buddy list's destructor"),
    0x10063680: ("o", "online: the buddy list, slot 0"),
    0x100621a0: ("o", "online: the buddy list, slot 4"),
    0x100622e0: ("o", "online: the buddy list's row fill (its 'online' / 'offline' strings made it (c))"),
    0x10062200: ("o", "online: the buddy list, slot 6"),
    0x10062e50: ("o", "online: the buddy list, slot 13"),
    # the stats list's categories 1-6 (0x1007de70 picks the row maker by
    # category; 0 = 0x1007d000 and 7 = 0x1007dbf0 were already (o)). The
    # page is BFHQ's, online only in the movie (engine.md "The stats page");
    # the rewrite's offline overview / player rows are a smaller set of its
    # own (MenuBindings::statsRows), not these 15-row makers.
    0x1007aec0: ("o", "online: the BFHQ stats list's category 1 rows (15 rows, %.4f / %d / percent / %.02f)"),
    0x1007b400: ("o", "online: the BFHQ stats list's category 2 rows"),
    0x1007b6f0: ("o", "online: the BFHQ stats list's category 3 rows"),
    0x1007bb10: ("o", "online: the BFHQ stats list's category 4 rows"),
    0x1007bd80: ("o", "online: the BFHQ stats list's category 5 rows"),
    0x1007c0e0: ("o", "online: the BFHQ stats list's category 6 rows"),
    0x10023590: ("u","the totalCount row of dice.General.getListEntries (0x10026e90, its only caller; unreached, see STATUS_LABEL)"),
    0x100d4270: ("u", "XML.load (no XML in the shipped movies)"),
    # --- never reached by the shipped movies (u): neither mainMenu.dfm nor
    # endOfRound.dfm has DefineSound / StartSound / DefineButtonSound /
    # DefineText / DefineFontInfo / DefineBitsJPEG2 / ImportAssets /
    # DefineButton tags or uses XML
    0x100d3cb0: ("u", "DefineSound's loader (no sound tags in the shipped movies)"),
    0x100d3f00: ("u", "StartSound's loader (no sound tags in the shipped movies)"),
    0x100d89c0: ("u", "a sound info record's reader (no sound tags in the shipped movies)"),
    0x100d9050: ("u", "DefineButtonSound's loader (no sound tags in the shipped movies)"),
    0x100ab730: ("u", "Sound.attachSound (no exported sounds in the shipped movies)"),
    0x100bfa40: ("u", "DefineText's text records (the shipped movies' text is all DefineEditText)"),
    0x100c0620: ("u", "DefineText's loader (no DefineText in the shipped movies)"),
    0x100b1440: ("u", "DefineFontInfo's loader (no DefineFontInfo in the shipped movies)"),
    0x100b1480: ("u", "DefineFontInfo2's loader (no DefineFontInfo2 in the shipped movies)"),
    0x100b4070: ("u", "DefineBitsJPEG2's loader (no DefineBitsJPEG2 in the shipped movies)"),
    0x100ba5b0: ("u", "ImportAssets' loader (no ImportAssets in the shipped movies)"),
    0x100b7ae0: ("u", "an imported character's resolver (no ImportAssets in the shipped movies)"),
    0x100d4230: ("u", "XML.ignoreWhite (no XML in the shipped movies)"),
    0x100d4510: ("u", "XML.onLoad (no XML in the shipped movies)"),
    0x100d4660: ("u", "XMLNode's members (no XML in the shipped movies)"),
    0x100d4920: ("u", "XML's loaded / status (no XML in the shipped movies)"),
    0x100d4bc0: ("u", "XMLNode's methods (no XML in the shipped movies)"),
    0x100d4e20: ("u", "XmlNode's class (no XML in the shipped movies)"),
    0x100d4e80: ("u", "XML's methods (no XML in the shipped movies)"),
}

# "Unreached": dice.lists.List's data source (dice.General.getListEntries
# 0x10026e90 and its revision, flush, filter and sort natives). Its only
# user, the AS class dice.UIs.Lists.ColumnList (sprite 821), is placed and
# attached by neither movie; the pages fill their lists through
# dice.bf2.List. Not needed, like the movies' unused tags (u).
STATUS_LABEL = {"Bound": "c", "Elsewhere": "c", "Online-only": "o", "External": "o", "Dev": "b", "Unreached": "u",
                "Missing": "d"}


def identifier_strings(strings):
    """The headline string rule: identifier-like strings only. gameswf's log
    and error text ("error: line_to needs two args") has no counterpart in
    a new player even where the code does the same."""
    return [s for s in strings if s.startswith("s_") or not re.search(r"\s|%|\\n", s)]


def _constructors(repo):
    """{object name: constructor address} from NativeTable.cpp's comments."""
    path = os.path.join(repo, "src", "UI", "Player", "NativeTable.cpp")
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*// (dice\.[A-Za-z0-9_.]+): 0x([0-9a-f]{8})", line)
            if m:
                out[m.group(1)] = int(m.group(2), 16)
    return out


def _statuses(repo):
    """{(object, member): status} from docs/uncited.md's natives table."""
    path = os.path.join(repo, "docs", "uncited.md")
    cols = ["Bound", "Elsewhere", "Online-only", "External", "Dev", "Unreached", "Missing"]
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\| `(dice\.[A-Za-z0-9_.]+)` \(`0x[0-9a-f]+`\) \|(.*)\|\s*$", line)
            if not m:
                continue
            cells = m.group(2).split("|")
            for col, cell in zip(cols, cells):
                cell = cell.strip()
                if not cell:
                    continue
                names = cell.split(":", 1)[1] if ":" in cell else cell
                for name in names.split(","):
                    name = name.strip().rstrip("*").strip()
                    if name:
                        out[(m.group(1), name.lower())] = col
    return out


def _decomp_bodies(repo):
    """{address: text} of every function in the decompilation."""
    import common  # noqa: E402 (tools/coverage is on the path)
    d = os.path.join(common.analysis_dir(MODULE, repo), "decomp")
    bodies = {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".c"):
            continue
        with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
            text = f.read()
        parts = re.split(r"^// ==== ([0-9a-f]{8}) ", text, flags=re.M)
        for i in range(1, len(parts) - 1, 2):
            bodies[int(parts[i], 16)] = parts[i + 1]
    return bodies


def native_members(repo):
    """{(object, member lowercased): body address} as the constructors
    register them: `local = FUN_x;` (the slot's function) before the
    member's name string."""
    bodies = _decomp_bodies(repo)
    out = {}
    for obj, ctor in _constructors(repo).items():
        text = bodies.get(ctor, "")
        fn = None
        for m in re.finditer(r"= (?:\([a-z_ ]+\*\))?&?(?:FUN|LAB|DAT)_([0-9a-f]{8});|FUN_100032b0\([^,]+,\"([^\"]+)\"\)", text):
            if m.group(1):
                fn = int(m.group(1), 16)
            elif fn is not None:
                out.setdefault((obj, m.group(2).lower()), fn)
                fn = None
    return out


def natives(repo):
    """{body address: (label, reason)} for the native members' bodies."""
    statuses = _statuses(repo)
    out = {}
    for key, fn in native_members(repo).items():
        status = statuses.get(key)
        if status is None or fn in out:
            continue
        out[fn] = (STATUS_LABEL[status], "native %s.%s (%s)" % (key[0], key[1], status.lower()))
    return out
