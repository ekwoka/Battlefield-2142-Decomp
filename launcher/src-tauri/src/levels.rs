// The levels a mod ships, read as the game reads them: each
// <game>/mods/<mod>/Levels/<level>/ folder, its server.zip's
// GameModes/<mode>/<size>/GamePlayObjects.con (what `+level`, `+gameMode`
// and `+mapSize` can load, as `scanMenuMaps` in src/Main/main.cpp lists
// them) and its Info/<level>.desc (the name, briefing and the modes in the
// order the menu's level scan meets them, MenuBindings::allGameModes).

use std::collections::HashMap;
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};

use serde::Serialize;

#[derive(Serialize, Clone, Debug, Default)]
pub struct LevelList {
    // Every folder in <game>/mods/, and the one listed.
    pub mods: Vec<String>,
    pub mod_name: String,
    // The mod's modes in the level scan's order (allGameModes), for a filter.
    pub modes: Vec<ModeName>,
    pub levels: Vec<Level>,
}

#[derive(Serialize, Clone, Debug, Default, PartialEq)]
pub struct ModeName {
    pub mode: String,
    // Its TEXT_<mode> in the mod's strings ("Conquest"), else the mode.
    pub title: String,
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct Level {
    // The folder's name: what +level takes.
    pub name: String,
    // As the menu shows it: the folder with underscores as spaces.
    pub title: String,
    // The briefing: its locid's English text, else the .desc's own.
    pub description: String,
    pub modes: Vec<LevelMode>,
    // AIPathFinding/Infantry.qtr in server.zip (or client.zip): the game
    // adds bots only on levels with it (BotManager::enabled).
    pub navmesh: bool,
    // What couldn't be read, for the list to show.
    pub problems: Vec<String>,
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct LevelMode {
    pub mode: String,
    pub title: String,
    pub sizes: Vec<LevelSize>,
}

#[derive(Serialize, Clone, Debug, Default, PartialEq)]
pub struct LevelSize {
    pub players: u32,
    // server.zip has its GamePlayObjects.con: the game can load it.
    pub playable: bool,
    // The .desc names it.
    pub listed: bool,
}

// What a level's Info/<level>.desc says.
#[derive(Debug, Default, PartialEq)]
pub struct Desc {
    pub name: String,
    pub briefing: String,
    pub briefing_key: String,
    // Each <mode type="...">, in the file's order, with its <maptype players="...">.
    pub modes: Vec<(String, Vec<u32>)>,
}

// The first `<tag>` (or `<tag ...>`, `<tag .../>`) from `from` on: its
// attributes, the text up to its `</tag>` (or the end), and where it ends.
fn element_from<'a>(xml: &'a str, tag: &str, mut from: usize) -> Option<(&'a str, &'a str, usize)> {
    let open = format!("<{tag}");
    let close = format!("</{tag}>");
    while let Some(at) = xml[from..].find(&open) {
        let start = from + at + open.len();
        let next = xml[start..].chars().next()?;
        if next == '>' || next == '/' || next.is_whitespace() {
            let open_end = start + xml[start..].find('>')?;
            let attributes = &xml[start..open_end];
            if attributes.trim_end().ends_with('/') {
                return Some((attributes, "", open_end + 1));
            }
            let body_start = open_end + 1;
            return Some(match xml[body_start..].find(&close) {
                Some(c) => (attributes, &xml[body_start..body_start + c], body_start + c + close.len()),
                None => (attributes, &xml[body_start..], xml.len()),
            });
        }
        from = start;
    }
    None
}

fn element<'a>(xml: &'a str, tag: &str) -> Option<(&'a str, &'a str)> {
    element_from(xml, tag, 0).map(|(a, b, _)| (a, b))
}

// Each `<tag ...>` in turn: its attributes and the text up to `</tag>`.
fn elements<'a>(xml: &'a str, tag: &str) -> Vec<(&'a str, &'a str)> {
    let mut out = Vec::new();
    let mut from = 0;
    while let Some((attributes, body, end)) = element_from(xml, tag, from) {
        out.push((attributes, body));
        from = end;
    }
    out
}

fn attribute(attributes: &str, name: &str) -> Option<String> {
    let mut from = 0;
    while let Some(at) = attributes[from..].find(name) {
        let start = from + at;
        let before_ok = start == 0 || attributes[..start].ends_with(char::is_whitespace);
        let rest = attributes[start + name.len()..].trim_start();
        if before_ok && let Some(rest) = rest.strip_prefix('=') {
            let rest = rest.trim_start();
            let rest = rest.strip_prefix('"')?;
            return Some(rest[..rest.find('"')?].to_string());
        }
        from = start + name.len();
    }
    None
}

// The .desc's parts, as parseLevelDescription (src/Main/EndOfRoundBindings.cpp) reads them.
pub fn parse_desc(xml: &str) -> Desc {
    let mut desc = Desc::default();
    if let Some((_, name)) = element(xml, "name") {
        desc.name = name.trim().to_string();
    }
    if let Some((attributes, briefing)) = element(xml, "briefing") {
        desc.briefing = briefing.trim().to_string();
        desc.briefing_key = attribute(attributes, "locid").unwrap_or_default();
    }
    for (attributes, body) in elements(xml, "mode") {
        let Some(mode) = attribute(attributes, "type") else { continue };
        let sizes = elements(body, "maptype")
            .into_iter()
            .map(|(a, _)| attribute(a, "players").and_then(|p| p.trim().parse().ok()).unwrap_or(0))
            .collect();
        desc.modes.push((mode, sizes));
    }
    desc
}

// Text as the game's files hold it: UTF-16 with a BOM, else UTF-8 (lossy).
fn decode(bytes: &[u8]) -> String {
    let utf16 = |bytes: &[u8], le: bool| {
        let units: Vec<u16> = bytes
            .as_chunks::<2>()
            .0
            .iter()
            .map(|&c| if le { u16::from_le_bytes(c) } else { u16::from_be_bytes(c) })
            .collect();
        String::from_utf16_lossy(&units)
    };
    match bytes {
        [0xff, 0xfe, rest @ ..] => utf16(rest, true),
        [0xfe, 0xff, rest @ ..] => utf16(rest, false),
        [0xef, 0xbb, 0xbf, rest @ ..] => String::from_utf8_lossy(rest).into_owned(),
        _ => String::from_utf8_lossy(bytes).into_owned(),
    }
}

// A file in `dir` whose name matches without regard to case (the game's
// file system is Windows').
fn find_file(dir: &Path, name: &str) -> Option<PathBuf> {
    let exact = dir.join(name);
    if exact.is_file() {
        return Some(exact);
    }
    std::fs::read_dir(dir)
        .ok()?
        .flatten()
        .find(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(name) && e.path().is_file())
        .map(|e| e.path())
}

// The mod's Localization/strings.csv, then strings_patch.csv over it: the
// English text (the second column) of the keys `wanted` accepts.
// Tab-separated UTF-16; a field in double quotes may hold tabs, line
// breaks and doubled quotes.
pub fn parse_strings(text: &str, wanted: &dyn Fn(&str) -> bool, out: &mut HashMap<String, String>) {
    let mut fields: Vec<String> = vec![String::new()];
    let mut quoted = false;
    let mut at_field_start = true;
    let mut chars = text.chars().peekable();
    let mut finish = |fields: &mut Vec<String>| {
        if fields.len() >= 2 && wanted(fields[0].trim()) {
            out.insert(fields[0].trim().to_string(), fields[1].clone());
        }
        fields.clear();
        fields.push(String::new());
    };
    while let Some(c) = chars.next() {
        if quoted {
            if c == '"' {
                if chars.peek() == Some(&'"') {
                    chars.next();
                    fields.last_mut().unwrap().push('"');
                } else {
                    quoted = false;
                }
            } else {
                fields.last_mut().unwrap().push(c);
            }
            continue;
        }
        match c {
            '"' if at_field_start => {
                quoted = true;
                at_field_start = false;
            }
            '\t' => {
                fields.push(String::new());
                at_field_start = true;
            }
            '\r' => {}
            '\n' => {
                finish(&mut fields);
                at_field_start = true;
            }
            _ => {
                // Only the first two columns matter; skip building the rest.
                if fields.len() <= 2 {
                    fields.last_mut().unwrap().push(c);
                }
                at_field_start = false;
            }
        }
    }
    finish(&mut fields);
}

fn load_strings(mod_dir: &Path) -> HashMap<String, String> {
    let wanted = |key: &str| key.starts_with("TEXT_") || key.starts_with("LOADINGSCREEN_MAPDESCRIPTION_");
    let mut out = HashMap::new();
    let dir = mod_dir.join("Localization");
    for name in ["strings.csv", "strings_patch.csv"] {
        if let Some(path) = find_file(&dir, name)
            && let Ok(bytes) = std::fs::read(path)
        {
            parse_strings(&decode(&bytes), &wanted, &mut out);
        }
    }
    out
}

// What a level's zips hold: its GameModes/<mode>/<size>/GamePlayObjects.con
// (mode, size) pairs in name order, and whether there's an infantry navmesh.
#[derive(Debug, Default)]
pub struct ZipScan {
    pub maps: Vec<(String, u32)>,
    pub navmesh: bool,
}

pub fn scan_entries<'a>(names: impl Iterator<Item = &'a str>) -> ZipScan {
    let mut scan = ZipScan::default();
    for name in names {
        let lower = name.replace('\\', "/").to_ascii_lowercase();
        if lower == "aipathfinding/infantry.qtr" {
            scan.navmesh = true;
        }
        // gamemodes/<mode>/<size>/gameplayobjects.con; the size as atoi
        // reads it ("16 No Vehicles" is 16).
        let original = name.replace('\\', "/");
        let parts: Vec<&str> = original.split('/').collect();
        if parts.len() == 4 && parts[0].eq_ignore_ascii_case("gamemodes") && parts[3].eq_ignore_ascii_case("gameplayobjects.con") {
            let digits: String = parts[2].trim_start().chars().take_while(char::is_ascii_digit).collect();
            let size = digits.parse().unwrap_or(0);
            if !scan.maps.iter().any(|(m, s)| m.eq_ignore_ascii_case(parts[1]) && *s == size) {
                scan.maps.push((parts[1].to_string(), size));
            }
        }
    }
    scan.maps.sort_by_key(|(mode, size)| (mode.to_ascii_lowercase(), *size));
    scan
}

fn zip_names(path: &Path) -> Result<Vec<String>, String> {
    let file = File::open(path).map_err(|e| e.to_string())?;
    let archive = zip::ZipArchive::new(file).map_err(|e| e.to_string())?;
    Ok(archive.file_names().map(str::to_string).collect())
}

// The modes and sizes a level offers: the .desc's modes in its order, then
// the ones only server.zip has (in name order); each with the sizes of
// both, marked with where they come from.
pub fn merge_modes(desc: Option<&Desc>, zip: &[(String, u32)], strings: &HashMap<String, String>) -> Vec<LevelMode> {
    let mut modes: Vec<LevelMode> = Vec::new();
    let entry = |modes: &mut Vec<LevelMode>, mode: &str| -> usize {
        if let Some(i) = modes.iter().position(|m| m.mode.eq_ignore_ascii_case(mode)) {
            return i;
        }
        modes.push(LevelMode { mode: mode.to_string(), title: mode_title(mode, strings), sizes: Vec::new() });
        modes.len() - 1
    };
    if let Some(desc) = desc {
        for (mode, sizes) in &desc.modes {
            if mode.is_empty() {
                continue;
            }
            let i = entry(&mut modes, mode);
            for &players in sizes {
                if !modes[i].sizes.iter().any(|s| s.players == players) {
                    modes[i].sizes.push(LevelSize { players, playable: false, listed: true });
                }
            }
        }
    }
    for (mode, players) in zip {
        let i = entry(&mut modes, mode);
        match modes[i].sizes.iter_mut().find(|s| s.players == *players) {
            Some(size) => size.playable = true,
            None => modes[i].sizes.push(LevelSize { players: *players, playable: true, listed: false }),
        }
    }
    for mode in &mut modes {
        mode.sizes.sort_by_key(|s| s.players);
    }
    modes
}

fn mode_title(mode: &str, strings: &HashMap<String, String>) -> String {
    strings.get(&format!("TEXT_{mode}")).filter(|t| !t.trim().is_empty()).cloned().unwrap_or_else(|| mode.to_string())
}

fn read_level(dir: &Path, name: &str, strings: &HashMap<String, String>) -> (Level, Option<Desc>) {
    let mut level = Level { name: name.to_string(), title: name.replace('_', " "), ..Default::default() };
    let desc_name = format!("{name}.desc");
    let desc = match find_file(&dir.join("Info"), &desc_name) {
        None => {
            level.problems.push(format!("no Info/{desc_name}"));
            None
        }
        Some(path) => match std::fs::read(&path) {
            Err(e) => {
                level.problems.push(format!("Info/{desc_name}: {e}"));
                None
            }
            Ok(bytes) => {
                let desc = parse_desc(&decode(&bytes));
                if desc.modes.is_empty() {
                    level.problems.push(format!("Info/{desc_name} names no modes"));
                    None
                } else {
                    Some(desc)
                }
            }
        },
    };
    if let Some(desc) = &desc {
        level.description = strings
            .get(&desc.briefing_key)
            .filter(|t| !t.trim().is_empty())
            .cloned()
            .unwrap_or_else(|| desc.briefing.clone());
    }
    let mut scan = ZipScan::default();
    match find_file(dir, "server.zip") {
        None => level.problems.push("no server.zip".into()),
        Some(path) => match zip_names(&path) {
            Ok(names) => scan = scan_entries(names.iter().map(String::as_str)),
            Err(e) => level.problems.push(format!("server.zip: {e}")),
        },
    }
    if !scan.navmesh
        && let Some(path) = find_file(dir, "client.zip")
        && let Ok(names) = zip_names(&path)
    {
        scan.navmesh = scan_entries(names.iter().map(String::as_str)).navmesh;
    }
    level.navmesh = scan.navmesh;
    if scan.maps.is_empty() && !level.problems.iter().any(|p| p.contains("server.zip")) {
        level.problems.push("server.zip has no GameModes/<mode>/<size>/GamePlayObjects.con".into());
    }
    level.modes = merge_modes(desc.as_ref(), &scan.maps, strings);
    (level, desc)
}

pub fn list_mods(game_dir: &Path) -> Vec<String> {
    let mut mods: Vec<String> = std::fs::read_dir(game_dir.join("mods"))
        .map(|dir| dir.flatten().filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default();
    mods.sort_by_key(|m| m.to_ascii_lowercase());
    mods
}

pub fn list_levels(game_dir: &Path, mod_name: &str) -> Result<LevelList, String> {
    if !game_dir.is_dir() {
        return Err(format!("{} isn't a folder", game_dir.display()));
    }
    let mods = list_mods(game_dir);
    if mods.is_empty() {
        return Err(format!("{} has no mods/ folder: is it the BF2142 directory?", game_dir.display()));
    }
    let mod_name = if mod_name.is_empty() { "bf2142" } else { mod_name };
    let mod_dir = game_dir.join("mods").join(mod_name);
    let levels_dir = mod_dir.join("Levels");
    let entries = std::fs::read_dir(&levels_dir).map_err(|e| format!("mods/{mod_name}/Levels: {e}"))?;
    let strings = load_strings(&mod_dir);
    let mut names: Vec<String> =
        entries.flatten().filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    // The level scan's order: FindFirstFileW on NTFS, upper-cased names.
    names.sort_by_key(|n| n.to_ascii_uppercase());
    let mut levels = Vec::new();
    let mut modes: Vec<ModeName> = Vec::new();
    for name in names {
        let (level, desc) = read_level(&levels_dir.join(&name), &name, &strings);
        // allGameModes: a level's .desc modes, else its server.zip's.
        let found: Vec<String> = match &desc {
            Some(d) => d.modes.iter().map(|(m, _)| m.clone()).collect(),
            None => level.modes.iter().map(|m| m.mode.clone()).collect(),
        };
        for mode in found {
            if !mode.is_empty() && !modes.iter().any(|m| m.mode.eq_ignore_ascii_case(&mode)) {
                modes.push(ModeName { title: mode_title(&mode, &strings), mode });
            }
        }
        levels.push(level);
    }
    Ok(LevelList { mods, mod_name: mod_name.to_string(), modes, levels })
}

// A picture of the level for a mode and size: Info/<mode>_<size>_preview.png,
// else its menuMap, else any of the mode's, else any; as a data: URL.
pub fn preview(game_dir: &Path, mod_name: &str, level: &str, mode: &str, size: u32) -> Option<String> {
    use base64::Engine;
    let info = game_dir.join("mods").join(mod_name).join("Levels").join(level).join("Info");
    let names: Vec<String> = std::fs::read_dir(&info).ok()?.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
    let lower: Vec<String> = names.iter().map(|n| n.to_ascii_lowercase()).collect();
    let mode = mode.to_ascii_lowercase();
    let exact = |n: &str| lower.iter().position(|l| l == n);
    let found = exact(&format!("{mode}_{size}_preview.png"))
        .or_else(|| exact(&format!("{mode}_{size}_menumap.png")))
        .or_else(|| lower.iter().position(|l| l.starts_with(&format!("{mode}_")) && l.ends_with("_preview.png")))
        .or_else(|| lower.iter().position(|l| l.starts_with(&format!("{mode}_")) && l.ends_with(".png")))
        .or_else(|| lower.iter().position(|l| l.ends_with("_preview.png")))?;
    let mut bytes = Vec::new();
    File::open(info.join(&names[found])).ok()?.read_to_end(&mut bytes).ok()?;
    Some(format!("data:image/png;base64,{}", base64::engine::general_purpose::STANDARD.encode(bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;

    const DESC: &str = "<map>\n\t<name> Camp_Gibraltar </name>\n\
        \t<briefing locid=\"LOADINGSCREEN_MAPDESCRIPTION_Camp_Gibraltar\">for modders</briefing>\n\
        \t<music> sound/music/Camp_Gibraltar.ogg </music>\n\t<modes>\n\
        \t\t<mode type=\"gpm_cq\">\n\
        \t\t\t<maptype players=\"16\" type=\"headon\" locid=\"GAMEMODE_DESCRIPTION_assault\">x</maptype>\n\
        \t\t\t<maptype players=\"32\" type=\"headon\" locid=\"GAMEMODE_DESCRIPTION_assault\">x</maptype>\n\
        \t\t</mode>\n\t\t<mode type=\"gpm_nv\">\n\
        \t\t\t<maptype players=\"16\" type=\"headon\">x</maptype>\n\t\t</mode>\n\t</modes>\n</map>";

    #[test]
    fn desc_parts() {
        let desc = parse_desc(DESC);
        assert_eq!(desc.name, "Camp_Gibraltar");
        assert_eq!(desc.briefing, "for modders");
        assert_eq!(desc.briefing_key, "LOADINGSCREEN_MAPDESCRIPTION_Camp_Gibraltar");
        assert_eq!(desc.modes, vec![("gpm_cq".to_string(), vec![16, 32]), ("gpm_nv".to_string(), vec![16])]);
    }

    #[test]
    fn desc_without_modes_or_garbage() {
        assert!(parse_desc("not xml at all").modes.is_empty());
        assert!(parse_desc("<map><modes><mode><maptype players=\"16\"/></mode></modes>").modes.is_empty());
        let desc = parse_desc("<modes><mode type=\"gpm_ti\"><maptype players=\"48\"/><maptype type=\"x\"/></mode>");
        assert_eq!(desc.modes, vec![("gpm_ti".to_string(), vec![48, 0])]);
    }

    #[test]
    fn utf16_desc() {
        let mut bytes = vec![0xff, 0xfe];
        bytes.extend(DESC.encode_utf16().flat_map(u16::to_le_bytes));
        assert_eq!(parse_desc(&decode(&bytes)).modes.len(), 2);
    }

    #[test]
    fn zip_entries() {
        let names = [
            "GameModes/gpm_cq/16/GamePlayObjects.con",
            "GameModes/gpm_cq/16 No Vehicles/GamePlayObjects.con",
            "GameModes/gpm_cq/32/GamePlayObjects.con",
            "GameModes/gpm_cq/32/Other.con",
            "gamemodes/GPM_TI/48/gameplayobjects.con",
            "GameModes/gpm_nv/16/GamePlayObjects.con",
            "AIPathFinding/Infantry.qtr",
        ];
        let scan = scan_entries(names.into_iter());
        assert!(scan.navmesh);
        let maps: Vec<(&str, u32)> = scan.maps.iter().map(|(m, s)| (m.as_str(), *s)).collect();
        assert_eq!(maps, [("gpm_cq", 16), ("gpm_cq", 32), ("gpm_nv", 16), ("GPM_TI", 48)]);
        assert!(!scan_entries(["AIPathFinding/Vehicle.qtr"].into_iter()).navmesh);
    }

    #[test]
    fn modes_merge_desc_first() {
        let desc = parse_desc(DESC);
        let zip = vec![("gpm_cq".to_string(), 16), ("gpm_cq".to_string(), 64), ("gpm_sp".to_string(), 16)];
        let strings = HashMap::from([("TEXT_gpm_cq".to_string(), "Conquest".to_string())]);
        let modes = merge_modes(Some(&desc), &zip, &strings);
        let order: Vec<&str> = modes.iter().map(|m| m.mode.as_str()).collect();
        assert_eq!(order, ["gpm_cq", "gpm_nv", "gpm_sp"]);
        assert_eq!(modes[0].title, "Conquest");
        assert_eq!(modes[1].title, "gpm_nv");
        assert_eq!(
            modes[0].sizes,
            [
                LevelSize { players: 16, playable: true, listed: true },
                LevelSize { players: 32, playable: false, listed: true },
                LevelSize { players: 64, playable: true, listed: false },
            ]
        );
        assert_eq!(modes[2].sizes, [LevelSize { players: 16, playable: true, listed: false }]);
        // No .desc: server.zip's alone.
        let modes = merge_modes(None, &zip, &strings);
        assert_eq!(modes.iter().map(|m| m.mode.as_str()).collect::<Vec<_>>(), ["gpm_cq", "gpm_sp"]);
    }

    #[test]
    fn strings_csv() {
        let text = "StringID\tEnglish\tGerman\r\nTEXT_gpm_cq\tConquest\tEroberung\r\n\
                    LOADINGSCREEN_MAPDESCRIPTION_X\t\"Line one\r\nwith \"\"quotes\"\"\tand a tab\"\tDe\r\n\
                    HUD_OTHER\tskip\r\nTEXT_gpm_ti\tTitan";
        let mut out = HashMap::new();
        parse_strings(text, &|k| k.starts_with("TEXT_") || k.starts_with("LOADINGSCREEN_"), &mut out);
        assert_eq!(out.len(), 3);
        assert_eq!(out["TEXT_gpm_cq"], "Conquest");
        assert_eq!(out["LOADINGSCREEN_MAPDESCRIPTION_X"], "Line one\r\nwith \"quotes\"\tand a tab");
        assert_eq!(out["TEXT_gpm_ti"], "Titan");
    }

    // A small game directory: one good level, one with a broken .desc and
    // no server.zip, one with a .desc in other case and a navmesh.
    fn fixture() -> PathBuf {
        use std::io::Write;
        let root = std::env::temp_dir().join(format!("bf2142-launcher-levels-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let levels = root.join("mods/bf2142/Levels");
        let zip_with = |path: PathBuf, names: &[&str]| {
            let mut zip = zip::ZipWriter::new(File::create(path).unwrap());
            for name in names {
                zip.start_file(*name, zip::write::SimpleFileOptions::default()).unwrap();
                zip.write_all(b"x").unwrap();
            }
            zip.finish().unwrap();
        };
        std::fs::create_dir_all(levels.join("Good_Level/Info")).unwrap();
        std::fs::write(levels.join("Good_Level/Info/Good_Level.desc"), DESC).unwrap();
        std::fs::write(levels.join("Good_Level/Info/gpm_cq_16_preview.png"), b"\x89PNG").unwrap();
        zip_with(levels.join("Good_Level/server.zip"), &["GameModes/gpm_cq/16/GamePlayObjects.con", "GameModes/gpm_sp/32/GamePlayObjects.con"]);
        std::fs::create_dir_all(levels.join("broken/Info")).unwrap();
        std::fs::write(levels.join("broken/Info/broken.desc"), "<map><name>broken").unwrap();
        std::fs::create_dir_all(levels.join("alpha/Info")).unwrap();
        std::fs::write(levels.join("alpha/Info/ALPHA.DESC"), "<modes><mode type=\"gpm_ti\"><maptype players=\"48\"/></mode></modes>").unwrap();
        zip_with(levels.join("alpha/server.zip"), &["GameModes/gpm_ti/48/GamePlayObjects.con", "AIPathFinding/Infantry.qtr"]);
        std::fs::create_dir_all(levels.join("nozip")).unwrap();
        std::fs::write(levels.join("nozip/server.zip"), b"not a zip").unwrap();
        std::fs::create_dir_all(root.join("mods/other/Levels")).unwrap();
        std::fs::create_dir_all(root.join("mods/bf2142/Localization")).unwrap();
        let mut strings = vec![0xff, 0xfe];
        strings.extend("StringID\tEnglish\r\nTEXT_gpm_cq\tConquest\r\n".encode_utf16().flat_map(u16::to_le_bytes));
        std::fs::write(root.join("mods/bf2142/Localization/strings.csv"), strings).unwrap();
        root
    }

    #[test]
    fn fixture_levels() {
        let root = fixture();
        let list = list_levels(&root, "").unwrap();
        assert_eq!(list.mods, ["bf2142", "other"]);
        assert_eq!(list.mod_name, "bf2142");
        let names: Vec<&str> = list.levels.iter().map(|l| l.name.as_str()).collect();
        assert_eq!(names, ["alpha", "broken", "Good_Level", "nozip"]);
        let alpha = &list.levels[0];
        assert!(alpha.navmesh && alpha.problems.is_empty(), "{alpha:?}");
        assert_eq!(alpha.modes[0].sizes, [LevelSize { players: 48, playable: true, listed: true }]);
        let broken = &list.levels[1];
        assert!(broken.modes.is_empty());
        assert!(broken.problems.iter().any(|p| p.contains("names no modes")), "{broken:?}");
        assert!(broken.problems.iter().any(|p| p.contains("no server.zip")), "{broken:?}");
        let good = &list.levels[2];
        assert_eq!(good.title, "Good Level");
        assert_eq!(good.description, "for modders");
        assert!(!good.navmesh);
        assert_eq!(good.modes.iter().map(|m| m.mode.as_str()).collect::<Vec<_>>(), ["gpm_cq", "gpm_nv", "gpm_sp"]);
        assert_eq!(good.modes[0].title, "Conquest");
        assert!(list.levels[3].problems.iter().any(|p| p.starts_with("server.zip:")), "{:?}", list.levels[3]);
        // The mod's modes: alpha's .desc, broken's nothing, Good_Level's .desc, then nothing new.
        assert_eq!(list.modes.iter().map(|m| m.mode.as_str()).collect::<Vec<_>>(), ["gpm_ti", "gpm_cq", "gpm_nv"]);
        assert!(preview(&root, "bf2142", "Good_Level", "gpm_cq", 16).unwrap().starts_with("data:image/png;base64,"));
        assert!(preview(&root, "bf2142", "alpha", "gpm_ti", 48).is_none());
        assert!(list_levels(&root, "other").unwrap().levels.is_empty());
        assert!(list_levels(&root, "missing").is_err());
        assert!(list_levels(&root.join("nothing"), "").is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    // The real game, when it's there (source/Battlefield 2142 or BF2142_GAME_DIR).
    #[test]
    fn the_game_levels() {
        let dir = match std::env::var("BF2142_GAME_DIR") {
            Ok(d) if !d.is_empty() => PathBuf::from(d),
            // The repository's, or the main checkout's from a worktree (.claude/worktrees/<name>/).
            _ => ["../..", "../../../../.."]
                .iter()
                .map(|up| Path::new(env!("CARGO_MANIFEST_DIR")).join(up).join("source/Battlefield 2142"))
                .find(|d| d.is_dir())
                .unwrap_or_default(),
        };
        if !dir.join("mods/bf2142/Levels").is_dir() {
            eprintln!("skipped: no game directory at {}", dir.display());
            return;
        }
        let list = list_levels(&dir, "bf2142").unwrap();
        for level in list.levels.iter().filter(|l| !l.problems.is_empty()) {
            eprintln!("{}: {}", level.name, level.problems.join("; "));
        }
        assert!(list.mods.iter().any(|m| m == "bf2142"));
        let minsk = list.levels.iter().find(|l| l.name == "Minsk").expect("Minsk");
        assert_eq!(minsk.modes.iter().map(|m| m.mode.as_str()).collect::<Vec<_>>()[..3], ["gpm_cq", "gpm_ti", "gpm_nv"]);
        let cq: Vec<u32> = minsk.modes[0].sizes.iter().filter(|s| s.playable).map(|s| s.players).collect();
        assert_eq!(cq, [16, 32, 64]);
        assert!(minsk.description.contains("Cold War"), "{}", minsk.description);
        assert_eq!(minsk.modes[0].title, "Conquest");
        let gib = list.levels.iter().find(|l| l.name == "Camp_Gibraltar").unwrap();
        assert!(!gib.navmesh);
        // "16 No Vehicles" is the same size as 16.
        assert_eq!(gib.modes[0].sizes.iter().filter(|s| s.players == 16).count(), 1);
        assert!(list.levels.iter().find(|l| l.name == "Suez_Canal").unwrap().navmesh);
        assert!(list.modes.iter().any(|m| m.mode == "gpm_ti" && m.title == "Titan"));
        // The_Island has no Info/ at all: it's listed with its server.zip modes and the problem.
        let island = list.levels.iter().find(|l| l.name == "The_Island").unwrap();
        assert!(island.problems.iter().any(|p| p.contains(".desc")));
        assert!(!island.modes.is_empty());
        assert!(preview(&dir, "bf2142", "Minsk", "gpm_cq", 16).is_some());
        // tgw_atom_day only has menumap pictures (lower case).
        assert!(preview(&dir, "bf2142", "tgw_atom_day", "gpm_cq", 32).is_some());
    }
}
