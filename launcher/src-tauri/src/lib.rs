// The BF2142 launcher's backend: it reads the game's options from the
// repository (README.md's option reference and the enhanced options in
// src/Scene/EnhancedSettings.h), builds the game with ninja and runs it,
// always from the repository root, streaming the output to the window.

use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use serde::Serialize;
use tauri::{AppHandle, Emitter, State};

#[derive(Serialize, Clone)]
struct Loaded {
    repo_root: String,
    default_game_dir: String,
    groups: Vec<Group>,
    enhanced: Vec<EnhancedFlag>,
}

#[derive(Serialize, Clone)]
struct Group {
    name: String,
    options: Vec<Opt>,
}

#[derive(Serialize, Clone)]
struct Opt {
    name: String,
    // What follows the flag in the README ("<dir>", "[1|2]", "1"), if anything.
    arg: Option<String>,
    // A literal value the README gives ("+widescreen 1"), else empty.
    default_value: String,
    description: String,
}

#[derive(Serialize, Clone)]
struct EnhancedFlag {
    name: String,
    default: bool,
    category: String,
    description: String,
}

#[derive(Serialize, Clone)]
struct LogLine {
    text: String,
    stderr: bool,
}

#[derive(Serialize, Clone)]
struct Finished {
    kind: String,
    ok: bool,
    code: Option<i32>,
}

// The one build or run going at a time.
#[derive(Default)]
struct Jobs {
    child: Arc<Mutex<Option<Child>>>,
    busy: Arc<AtomicBool>,
}

// The repository: the working directory when the launcher is started from
// it, else the one this crate sits in (launcher/src-tauri).
fn repo_root() -> PathBuf {
    let is_repo = |p: &Path| p.join("CMakeLists.txt").is_file() && p.join("src/Main").is_dir();
    if let Ok(cwd) = std::env::current_dir()
        && is_repo(&cwd)
    {
        return cwd;
    }
    let here = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    here.canonicalize().unwrap_or(here)
}

fn resolve(dir: &str) -> PathBuf {
    let path = Path::new(dir.trim());
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        repo_root().join(path)
    }
}

fn binary_path(build_dir: &str) -> PathBuf {
    let name = if cfg!(windows) { "bf2142.exe" } else { "bf2142" };
    resolve(build_dir).join("src/Main").join(name)
}

// Splits a markdown table row on its unescaped pipes ("\|" is a literal one).
fn table_cells(line: &str) -> Vec<String> {
    let mut cells = vec![String::new()];
    let mut chars = line.trim().trim_start_matches('|').chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\\' if chars.peek() == Some(&'|') => {
                cells.last_mut().unwrap().push('|');
                chars.next();
            }
            '|' => cells.push(String::new()),
            _ => cells.last_mut().unwrap().push(c),
        }
    }
    cells.iter().map(|c| c.trim().to_string()).filter(|c| !c.is_empty()).collect()
}

// The README's light markdown as plain text: no backticks or bold, links as
// their text.
fn plain(text: &str) -> String {
    let mut out = String::new();
    let mut rest = text;
    while let Some(open) = rest.find('[') {
        let Some(mid) = rest[open..].find("](") else { break };
        let Some(close) = rest[open + mid..].find(')') else { break };
        out.push_str(&rest[..open]);
        out.push_str(&rest[open + 1..open + mid]);
        rest = &rest[open + mid + close + 1..];
    }
    out.push_str(rest);
    out.replace("**", "").replace('`', "")
}

// "Option reference" in README.md: each bold line starts a group, each
// table row's first cell names one or more flags in backticks.
fn parse_readme(text: &str) -> Vec<Group> {
    let mut groups: Vec<Group> = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut inside = false;
    for line in text.lines() {
        if line.starts_with("### ") {
            if inside {
                break;
            }
            inside = line.trim() == "### Option reference";
            continue;
        }
        if !inside {
            continue;
        }
        if let Some(rest) = line.strip_prefix("**") {
            let name = rest.split("**").next().unwrap_or(rest).trim().to_string();
            groups.push(Group { name, options: Vec::new() });
            continue;
        }
        if !line.starts_with("| `") {
            continue;
        }
        let cells = table_cells(line);
        if cells.len() < 2 || groups.is_empty() {
            continue;
        }
        let description = plain(&cells[1]);
        for (i, flag) in cells[0].split('`').enumerate() {
            if i % 2 == 0 {
                continue; // between the backticks
            }
            let flag = flag.trim();
            let (name, rest) = match flag.split_once(char::is_whitespace) {
                Some((n, r)) => (n.to_string(), r.trim().to_string()),
                None => (flag.to_string(), String::new()),
            };
            if !(name.starts_with("--") || name.starts_with('+')) || name == "--game" || name == "--enhanced" {
                continue;
            }
            if !seen.insert(name.clone()) {
                continue; // a flag listed again under a later group
            }
            let literal = !rest.is_empty() && !rest.contains(['<', '[', '|', '"', ':', '/', ',']);
            let default_value = if literal { rest.clone() } else { String::new() };
            let arg = (!rest.is_empty()).then_some(rest);
            groups.last_mut().unwrap().options.push(Opt { name, arg, default_value, description: description.clone() });
        }
    }
    groups.retain(|g| !g.options.is_empty());
    groups
}

// EnhancedSettings.h: "bool name = default; // enhanced.name: what it does",
// under comments naming each part ("// Gameplay, also only with ...:").
fn parse_enhanced(text: &str) -> Vec<EnhancedFlag> {
    let mut flags = Vec::new();
    let mut category = String::new();
    let mut inside = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with("struct EnhancedSettings") {
            inside = true;
            continue;
        }
        if !inside {
            continue;
        }
        if line.starts_with("};") {
            break;
        }
        if let Some(comment) = line.strip_prefix("// ") {
            if comment.ends_with(':') {
                category = comment.split([',', ':', '(']).next().unwrap_or(comment).trim().to_string();
            }
            continue;
        }
        let Some(decl) = line.strip_prefix("bool ") else { continue };
        let Some((name, rest)) = decl.split_once('=') else { continue };
        let name = name.trim().to_string();
        if name == "enabled" {
            continue;
        }
        let default = rest.trim_start().starts_with("true");
        let description = rest
            .split_once("//")
            .map(|(_, c)| c.trim())
            .map(|c| c.split_once(": ").map(|(_, d)| d).unwrap_or(c).to_string())
            .unwrap_or_default();
        flags.push(EnhancedFlag { name, default, category: category.clone(), description });
    }
    flags
}

#[tauri::command]
fn load_options() -> Result<Loaded, String> {
    let root = repo_root();
    let readme = std::fs::read_to_string(root.join("README.md")).map_err(|e| format!("README.md: {e}"))?;
    let enhanced = std::fs::read_to_string(root.join("src/Scene/EnhancedSettings.h"))
        .map_err(|e| format!("src/Scene/EnhancedSettings.h: {e}"))?;
    let default_game_dir = match std::env::var("BF2142_GAME_DIR") {
        Ok(dir) if !dir.is_empty() => dir,
        _ if root.join("source/Battlefield 2142").is_dir() => "source/Battlefield 2142".to_string(),
        _ => String::new(),
    };
    Ok(Loaded {
        repo_root: root.display().to_string(),
        default_game_dir,
        groups: parse_readme(&readme),
        enhanced: parse_enhanced(&enhanced),
    })
}

#[tauri::command]
fn binary_exists(build_dir: String) -> bool {
    binary_path(&build_dir).is_file()
}

fn emit_line(app: &AppHandle, text: String, stderr: bool) {
    let _ = app.emit("log", LogLine { text, stderr });
}

fn describe(cmd: &Command) -> String {
    let quote = |s: &str| if s.is_empty() || s.contains(char::is_whitespace) { format!("\"{s}\"") } else { s.to_string() };
    let mut parts = vec![quote(&cmd.get_program().to_string_lossy())];
    parts.extend(cmd.get_args().map(|a| quote(&a.to_string_lossy())));
    parts.join(" ")
}

fn pipe(app: AppHandle, stream: Option<impl Read + Send + 'static>, stderr: bool) -> Option<thread::JoinHandle<()>> {
    let stream = stream?;
    Some(thread::spawn(move || {
        let mut reader = BufReader::new(stream);
        let mut buf = Vec::new();
        while reader.read_until(b'\n', &mut buf).map(|n| n > 0).unwrap_or(false) {
            let text = String::from_utf8_lossy(&buf).trim_end_matches(['\r', '\n']).to_string();
            emit_line(&app, text, stderr);
            buf.clear();
        }
    }))
}

// Runs the steps in order from the repository root, each until it exits,
// stopping at the first that fails; then "job-finished".
fn start_job(app: AppHandle, jobs: &Jobs, kind: &str, steps: Vec<Command>) -> Result<(), String> {
    if jobs.busy.swap(true, Ordering::SeqCst) {
        return Err("a build or run is already going".into());
    }
    let slot = jobs.child.clone();
    let busy = jobs.busy.clone();
    let kind = kind.to_string();
    let root = repo_root();
    thread::spawn(move || {
        let mut ok = true;
        let mut code = None;
        for mut cmd in steps {
            emit_line(&app, format!("$ {}", describe(&cmd)), false);
            cmd.current_dir(&root).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
            let mut child = match cmd.spawn() {
                Ok(child) => child,
                Err(e) => {
                    emit_line(&app, format!("could not start: {e}"), true);
                    ok = false;
                    break;
                }
            };
            let out = pipe(app.clone(), child.stdout.take(), false);
            let err = pipe(app.clone(), child.stderr.take(), true);
            *slot.lock().unwrap() = Some(child);
            let status = loop {
                {
                    let mut guard = slot.lock().unwrap();
                    match guard.as_mut().map(|c| c.try_wait()) {
                        Some(Ok(None)) => {}
                        Some(Ok(Some(status))) => {
                            *guard = None;
                            break Some(status);
                        }
                        _ => {
                            *guard = None;
                            break None;
                        }
                    }
                }
                thread::sleep(Duration::from_millis(100));
            };
            for handle in [out, err].into_iter().flatten() {
                let _ = handle.join();
            }
            code = status.and_then(|s| s.code());
            if !status.is_some_and(|s| s.success()) {
                ok = false;
                break;
            }
        }
        busy.store(false, Ordering::SeqCst);
        let _ = app.emit("job-finished", Finished { kind, ok, code });
    });
    Ok(())
}

// Configures the build directory with CMake (Release, Ninja) the first
// time, then builds the game with ninja.
#[tauri::command]
fn build(app: AppHandle, jobs: State<Jobs>, build_dir: String) -> Result<(), String> {
    let dir = build_dir.trim().to_string();
    if dir.is_empty() {
        return Err("no build directory".into());
    }
    let mut steps = Vec::new();
    if !resolve(&dir).join("build.ninja").is_file() {
        let mut configure = Command::new("cmake");
        configure.args(["-S", ".", "-B", &dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]);
        steps.push(configure);
    }
    let mut ninja = Command::new("ninja");
    ninja.args(["-C", &dir, "src/Main/bf2142"]);
    steps.push(ninja);
    start_job(app, &jobs, "build", steps)
}

#[tauri::command]
fn run_game(app: AppHandle, jobs: State<Jobs>, build_dir: String, args: Vec<String>) -> Result<(), String> {
    let binary = binary_path(&build_dir);
    if !binary.is_file() {
        return Err(format!("{} isn't built yet", binary.display()));
    }
    let mut cmd = Command::new(binary);
    cmd.args(args);
    start_job(app, &jobs, "run", vec![cmd])
}

// A folder picker that opens at `start`. A folder inside the repository comes
// back relative to it, like the default "source/Battlefield 2142"; None when
// the picker is cancelled. Async, so the dialog doesn't block the main thread.
#[tauri::command]
async fn pick_dir(app: AppHandle, start: String) -> Option<String> {
    use tauri_plugin_dialog::DialogExt;
    let start = resolve(&start);
    let mut dialog = app.dialog().file().set_title("The BF2142 directory");
    if start.is_dir() {
        dialog = dialog.set_directory(&start);
    }
    let picked = dialog.blocking_pick_folder()?.into_path().ok()?;
    let root = repo_root();
    Some(match picked.strip_prefix(&root) {
        Ok(rel) if !rel.as_os_str().is_empty() => rel.display().to_string(),
        _ => picked.display().to_string(),
    })
}

#[tauri::command]
fn stop(jobs: State<Jobs>) -> Result<(), String> {
    if let Some(child) = jobs.child.lock().unwrap().as_mut() {
        child.kill().map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Jobs::default())
        .invoke_handler(tauri::generate_handler![load_options, binary_exists, build, run_game, pick_dir, stop])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn readme_rows() {
        let text = "### Option reference\n\n**Game and level**\n\n| Option | What it does |\n|---|---|\n\
                    | `--game <dir>` | The install. |\n| `--camera-mode first\\|third` | A view. |\n\
                    | `--nosound` / `--sound` | No audio / audio. |\n| `+widescreen 1` | Wide [modes](#x). |\n\
                    **Joining a game**\n\n| `--play [1\\|2]` | Spawn. |\n| `--play [1\\|2]` | Again. |\n\
                    ### Environment variables\n| `--late` | Not an option. |\n";
        let groups = parse_readme(text);
        assert_eq!(groups.len(), 2);
        let names: Vec<_> = groups[0].options.iter().map(|o| o.name.as_str()).collect();
        assert_eq!(names, ["--camera-mode", "--nosound", "--sound", "+widescreen"]);
        assert_eq!(groups[0].options[0].arg.as_deref(), Some("first|third"));
        assert_eq!(groups[0].options[0].default_value, "");
        assert_eq!(groups[0].options[3].default_value, "1");
        assert_eq!(groups[0].options[3].description, "Wide modes.");
        assert_eq!(groups[1].options.len(), 1);
        assert_eq!(groups[1].options[0].arg.as_deref(), Some("[1|2]"));
    }

    #[test]
    fn enhanced_flags() {
        let text = "struct EnhancedSettings {\n\tbool enabled = false; // enhanced.enabled\n\
                    \t// The render passes, each only with enhanced.enabled (src/Render/Enhanced):\n\
                    \tbool taa = true;      // enhanced.taa: temporal anti-aliasing\n\
                    \t// Gameplay, also only with enhanced.enabled; changes the simulation:\n\
                    \tbool cornerSteer = false; // enhanced.cornerSteer: experimental\n};\n\tbool after = true;\n";
        let flags = parse_enhanced(text);
        assert_eq!(flags.len(), 2);
        assert_eq!((flags[0].name.as_str(), flags[0].default, flags[0].category.as_str()), ("taa", true, "The render passes"));
        assert_eq!(flags[0].description, "temporal anti-aliasing");
        assert_eq!((flags[1].name.as_str(), flags[1].default, flags[1].category.as_str()), ("cornerSteer", false, "Gameplay"));
    }

    #[test]
    fn the_repository_parses() {
        let root = repo_root();
        let groups = parse_readme(&std::fs::read_to_string(root.join("README.md")).unwrap());
        assert!(groups.len() >= 8, "{} groups", groups.len());
        assert!(groups.iter().flat_map(|g| &g.options).any(|o| o.name == "+level"));
        let flags = parse_enhanced(&std::fs::read_to_string(root.join("src/Scene/EnhancedSettings.h")).unwrap());
        assert!(flags.iter().any(|f| f.name == "singleplayAllModes" && f.default));
    }
}
