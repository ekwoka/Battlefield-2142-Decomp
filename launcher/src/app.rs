// The BF2142 launcher's window: every option the README lists, the
// enhanced options from EnhancedSettings.h, and Build / Run / Stop. The
// choices are kept in the window's local storage between launches.

use std::collections::BTreeMap;

use leptos::html;
use leptos::prelude::*;
use leptos::task::spawn_local;
use serde::{Deserialize, Serialize};
use wasm_bindgen::prelude::*;

#[wasm_bindgen]
extern "C" {
    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "core"], catch)]
    async fn invoke(cmd: &str, args: JsValue) -> Result<JsValue, JsValue>;
    #[wasm_bindgen(js_namespace = ["window", "__TAURI__", "event"])]
    async fn listen(event: &str, handler: &Closure<dyn FnMut(JsValue)>) -> JsValue;
}

async fn call<A: Serialize, R: for<'de> Deserialize<'de>>(cmd: &str, args: &A) -> Result<R, String> {
    let args = serde_wasm_bindgen::to_value(args).map_err(|e| e.to_string())?;
    let value = invoke(cmd, args).await.map_err(|e| e.as_string().unwrap_or_else(|| format!("{e:?}")))?;
    serde_wasm_bindgen::from_value(value).map_err(|e| e.to_string())
}

// What the backend reads from the repository (src-tauri/src/main.rs).
#[derive(Deserialize, Clone)]
struct Loaded {
    repo_root: String,
    default_game_dir: String,
    groups: Vec<Group>,
    enhanced: Vec<EnhancedFlag>,
}

#[derive(Deserialize, Clone)]
struct Group {
    name: String,
    options: Vec<Opt>,
}

#[derive(Deserialize, Clone)]
struct Opt {
    name: String,
    arg: Option<String>,
    default_value: String,
    description: String,
}

#[derive(Deserialize, Clone)]
struct EnhancedFlag {
    name: String,
    default: bool,
    category: String,
    description: String,
}

// A mod's levels (src-tauri/src/levels.rs).
#[derive(Deserialize, Clone, PartialEq, Default)]
struct LevelList {
    mods: Vec<String>,
    mod_name: String,
    modes: Vec<ModeName>,
    levels: Vec<Level>,
}

#[derive(Deserialize, Clone, PartialEq)]
struct ModeName {
    mode: String,
    title: String,
}

#[derive(Deserialize, Clone, PartialEq)]
struct Level {
    name: String,
    title: String,
    description: String,
    modes: Vec<LevelMode>,
    navmesh: bool,
    problems: Vec<String>,
}

#[derive(Deserialize, Clone, PartialEq)]
struct LevelMode {
    mode: String,
    title: String,
    sizes: Vec<LevelSize>,
}

#[derive(Deserialize, Clone, PartialEq)]
struct LevelSize {
    players: u32,
    playable: bool,
    listed: bool,
}

impl Level {
    fn playable(&self) -> bool {
        self.modes.iter().any(|m| m.sizes.iter().any(|s| s.playable))
    }
}

impl LevelMode {
    fn playable(&self, players: u32) -> bool {
        self.sizes.iter().any(|s| s.players == players && s.playable)
    }

    // The size to keep when this mode is chosen: the current one if the
    // game can load it, else 32 (the game's default), else the first.
    fn size_for(&self, current: u32) -> u32 {
        [current, 32]
            .into_iter()
            .find(|&n| self.playable(n))
            .or_else(|| self.sizes.iter().find(|s| s.playable).map(|s| s.players))
            .or_else(|| self.sizes.first().map(|s| s.players))
            .unwrap_or(current)
    }
}

#[derive(Deserialize)]
struct TauriEvent<T> {
    payload: T,
}

#[derive(Deserialize)]
struct LogLine {
    text: String,
    stderr: bool,
}

#[derive(Deserialize)]
struct Finished {
    kind: String,
    ok: bool,
    code: Option<i32>,
}

#[derive(Serialize)]
struct NoArgs {}

#[derive(Serialize)]
struct PickArgs {
    start: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DirArgs {
    build_dir: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct LevelArgs {
    game_dir: String,
    mod_name: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PreviewArgs {
    game_dir: String,
    mod_name: String,
    level: String,
    mode: String,
    size: u32,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RunArgs {
    build_dir: String,
    args: Vec<String>,
}

// The choices kept between launches.
#[derive(Serialize, Deserialize, Default)]
struct Saved {
    game_dir: Option<String>,
    build_dir: Option<String>,
    enhanced: bool,
    enhanced_flags: BTreeMap<String, bool>,
    options: BTreeMap<String, SavedOpt>,
    extra: String,
    // What Run opens: "menu" (also when unset), "view" or "none".
    #[serde(default)]
    start: String,
    // The map picker's choices.
    #[serde(default)]
    map: SavedMap,
}

#[derive(Serialize, Deserialize, Default, Clone)]
#[serde(default)]
struct SavedMap {
    mod_name: String,
    level: String,
    game_mode: String,
    map_size: u32,
    bots: u32,
    // 0..1 for aiSettings.setBotSkill; empty for the AI scripts' own.
    bot_skill: String,
    // Minutes; empty or 0 for none.
    time_limit: String,
    filter: String,
    mode_filter: String,
    navmesh_only: bool,
}

// The options the map picker gives itself; the option groups leave them out.
const MAP_OPTIONS: &[&str] = &["+level", "+gameMode", "+mapSize", "+modPath", "--bots", "--time-limit"];

// The map picker's state.
#[derive(Clone, Copy)]
struct MapChoice {
    mod_name: RwSignal<String>,
    level: RwSignal<String>,
    game_mode: RwSignal<String>,
    map_size: RwSignal<u32>,
    bots: RwSignal<u32>,
    bot_skill: RwSignal<String>,
    time_limit: RwSignal<String>,
    filter: RwSignal<String>,
    mode_filter: RwSignal<String>,
    navmesh_only: RwSignal<bool>,
}

impl MapChoice {
    fn new(saved: &SavedMap) -> Self {
        MapChoice {
            mod_name: RwSignal::new(if saved.mod_name.is_empty() { "bf2142".to_string() } else { saved.mod_name.clone() }),
            level: RwSignal::new(saved.level.clone()),
            game_mode: RwSignal::new(saved.game_mode.clone()),
            map_size: RwSignal::new(saved.map_size),
            bots: RwSignal::new(saved.bots),
            bot_skill: RwSignal::new(saved.bot_skill.clone()),
            time_limit: RwSignal::new(saved.time_limit.clone()),
            filter: RwSignal::new(saved.filter.clone()),
            mode_filter: RwSignal::new(saved.mode_filter.clone()),
            navmesh_only: RwSignal::new(saved.navmesh_only),
        }
    }

    fn saved(&self) -> SavedMap {
        SavedMap {
            mod_name: self.mod_name.get(),
            level: self.level.get(),
            game_mode: self.game_mode.get(),
            map_size: self.map_size.get(),
            bots: self.bots.get(),
            bot_skill: self.bot_skill.get(),
            time_limit: self.time_limit.get(),
            filter: self.filter.get(),
            mode_filter: self.mode_filter.get(),
            navmesh_only: self.navmesh_only.get(),
        }
    }

    // The arguments for the mod and, unless the menu is to pick the map, a
    // direct start into the chosen level.
    fn args(&self, menu: bool) -> Vec<String> {
        let mut args = Vec::new();
        let mod_name = self.mod_name.get();
        if !mod_name.is_empty() && mod_name != "bf2142" {
            args.extend(["+modPath".to_string(), format!("mods/{mod_name}")]);
        }
        let level = self.level.get();
        if menu || level.is_empty() {
            return args;
        }
        args.extend(["+level".to_string(), level]);
        let mode = self.game_mode.get();
        if !mode.is_empty() {
            args.extend(["+gameMode".to_string(), mode]);
        }
        let size = self.map_size.get();
        if size > 0 {
            args.extend(["+mapSize".to_string(), size.to_string()]);
        }
        let bots = self.bots.get();
        if bots > 0 {
            args.extend(["--bots".to_string(), bots.to_string()]);
            if let Ok(skill) = self.bot_skill.get().trim().parse::<f32>() {
                args.extend(["--console".to_string(), format!("aiSettings.setBotSkill {}", skill.clamp(0.0, 1.0))]);
            }
        }
        if let Ok(minutes) = self.time_limit.get().trim().parse::<f32>()
            && minutes > 0.0
        {
            args.extend(["--time-limit".to_string(), ((minutes * 60.0).round() as i64).to_string()]);
        }
        args
    }

    // Chooses a level: the current mode if it has the level, else its first
    // the game can load; the size likewise.
    fn choose(&self, level: &Level) {
        self.level.set(level.name.clone());
        let current = self.game_mode.get_untracked();
        let mode = level
            .modes
            .iter()
            .find(|m| m.mode.eq_ignore_ascii_case(&current) && m.sizes.iter().any(|s| s.playable))
            .or_else(|| level.modes.iter().find(|m| m.sizes.iter().any(|s| s.playable)))
            .or_else(|| level.modes.first());
        if let Some(mode) = mode {
            self.choose_mode(mode);
        }
    }

    fn choose_mode(&self, mode: &LevelMode) {
        self.game_mode.set(mode.mode.clone());
        self.map_size.set(mode.size_for(self.map_size.get_untracked()));
    }
}

#[derive(Serialize, Deserialize, Default, Clone)]
struct SavedOpt {
    on: bool,
    value: String,
}

const STORAGE_KEY: &str = "bf2142-launcher";
const MAX_LOG_LINES: usize = 5000;

fn storage() -> Option<web_sys::Storage> {
    web_sys::window()?.local_storage().ok()?
}

fn load_saved() -> Saved {
    storage()
        .and_then(|s| s.get_item(STORAGE_KEY).ok().flatten())
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save(saved: &Saved) {
    if let (Some(s), Ok(text)) = (storage(), serde_json::to_string(saved)) {
        let _ = s.set_item(STORAGE_KEY, &text);
    }
}

// Splits the extra arguments as a shell would: on spaces, with '…' and "…"
// keeping their spaces.
fn split_args(text: &str) -> Vec<String> {
    let mut args = Vec::new();
    let mut current = String::new();
    let mut quote = None;
    let mut started = false;
    for c in text.chars() {
        match (quote, c) {
            (Some(q), c) if c == q => quote = None,
            (Some(_), c) => current.push(c),
            (None, '"' | '\'') => {
                quote = Some(c);
                started = true;
            }
            (None, c) if c.is_whitespace() => {
                if started || !current.is_empty() {
                    args.push(std::mem::take(&mut current));
                }
                started = false;
            }
            (None, c) => current.push(c),
        }
    }
    if started || !current.is_empty() {
        args.push(current);
    }
    args
}

fn shell_quote(arg: &str) -> String {
    if !arg.is_empty() && arg.chars().all(|c| c.is_ascii_alphanumeric() || "-_./+:=@,%".contains(c)) {
        arg.to_string()
    } else {
        format!("'{}'", arg.replace('\'', "'\\''"))
    }
}

#[derive(Clone)]
struct OptState {
    opt: Opt,
    on: RwSignal<bool>,
    value: RwSignal<String>,
}

#[derive(Clone)]
struct FlagState {
    flag: EnhancedFlag,
    on: RwSignal<bool>,
}

#[component]
pub fn App() -> impl IntoView {
    let loaded = RwSignal::new(None::<Result<Loaded, String>>);
    spawn_local(async move {
        loaded.set(Some(call::<_, Loaded>("load_options", &NoArgs {}).await));
    });
    move || match loaded.get() {
        None => view! { <p>"Reading the options…"</p> }.into_any(),
        Some(Err(e)) => view! { <p class="error">"Couldn't read the options: " {e}</p> }.into_any(),
        Some(Ok(l)) => view! { <Launcher loaded=l /> }.into_any(),
    }
}

#[component]
fn Launcher(loaded: Loaded) -> impl IntoView {
    let saved = load_saved();
    let game_dir = RwSignal::new(saved.game_dir.clone().unwrap_or_else(|| loaded.default_game_dir.clone()));
    let build_dir = RwSignal::new(saved.build_dir.clone().unwrap_or_else(|| "build/release".to_string()));
    let enhanced = RwSignal::new(saved.enhanced);
    let extra = RwSignal::new(saved.extra.clone());
    let start_with = RwSignal::new(if saved.start.is_empty() { "menu".to_string() } else { saved.start.clone() });
    let map = MapChoice::new(&saved.map);
    let flags: Vec<FlagState> = loaded
        .enhanced
        .iter()
        .map(|f| FlagState { on: RwSignal::new(*saved.enhanced_flags.get(&f.name).unwrap_or(&f.default)), flag: f.clone() })
        .collect();
    let groups: Vec<(String, Vec<OptState>)> = loaded
        .groups
        .iter()
        .map(|g| {
            let options = g
                .options
                .iter()
                .filter(|o| !MAP_OPTIONS.contains(&o.name.as_str()))
                .map(|o| {
                    let kept = saved.options.get(&o.name).cloned().unwrap_or_else(|| SavedOpt { on: false, value: o.default_value.clone() });
                    OptState { opt: o.clone(), on: RwSignal::new(kept.on), value: RwSignal::new(kept.value) }
                })
                .collect();
            (g.name.clone(), options)
        })
        .filter(|(_, options): &(String, Vec<OptState>)| !options.is_empty())
        .collect();
    let options: Vec<OptState> = groups.iter().flat_map(|(_, o)| o.clone()).collect();

    // The command line, in the order: the game, the start, the map, the
    // options, enhanced, extra.
    let args = {
        let options = options.clone();
        let flags = flags.clone();
        Memo::new(move |_| {
            let mut args = Vec::new();
            let dir = game_dir.get();
            if !dir.trim().is_empty() {
                args.push("--game".to_string());
                args.push(dir.trim().to_string());
            }
            // Without --menu, --view, --play or --connect the game opens no
            // window: it loads, prints its summary and exits.
            let mode = match start_with.get().as_str() {
                "view" => Some("--view"),
                "none" => None,
                _ => Some("--menu"),
            };
            if let Some(mode) = mode
                && !options.iter().any(|o| o.on.get() && o.opt.name == mode)
            {
                args.push(mode.to_string());
            }
            // The mod, and the level to start in (not when the menu picks it).
            let menu = mode == Some("--menu") || options.iter().any(|o| o.on.get() && o.opt.name == "--menu");
            args.extend(map.args(menu));
            for o in &options {
                if o.on.get() {
                    args.push(o.opt.name.clone());
                    let value = o.value.get();
                    if o.opt.arg.is_some() && !value.trim().is_empty() {
                        args.push(value.trim().to_string());
                    }
                }
            }
            if enhanced.get() {
                args.push("--enhanced".to_string());
                for f in &flags {
                    let on = f.on.get();
                    if on != f.flag.default {
                        args.push(format!("+enhanced.{}", f.flag.name));
                        args.push(if on { "1" } else { "0" }.to_string());
                    }
                }
            }
            args.extend(split_args(&extra.get()));
            args
        })
    };

    // Kept on every change.
    {
        let options = options.clone();
        let flags = flags.clone();
        Effect::new(move |_| {
            save(&Saved {
                game_dir: Some(game_dir.get()),
                build_dir: Some(build_dir.get()),
                enhanced: enhanced.get(),
                enhanced_flags: flags.iter().map(|f| (f.flag.name.clone(), f.on.get())).collect(),
                options: options.iter().map(|o| (o.opt.name.clone(), SavedOpt { on: o.on.get(), value: o.value.get() })).collect(),
                extra: extra.get(),
                start: start_with.get(),
                map: map.saved(),
            });
        });
    }

    let built = RwSignal::new(false);
    let running = RwSignal::new(None::<String>);
    let log = RwSignal::new(Vec::<(String, bool)>::new());
    let push_log = move |text: String, stderr: bool| {
        log.update(|l| {
            l.push((text, stderr));
            if l.len() > MAX_LOG_LINES {
                let over = l.len() - MAX_LOG_LINES;
                l.drain(..over);
            }
        })
    };
    let check_built = move || {
        let dir = build_dir.get_untracked();
        spawn_local(async move {
            built.set(call::<_, bool>("binary_exists", &DirArgs { build_dir: dir }).await.unwrap_or(false));
        });
    };
    Effect::new(move |_| {
        build_dir.track();
        check_built();
    });

    // The backend's output and the end of each build or run.
    spawn_local(async move {
        let on_log = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
            if let Ok(e) = serde_wasm_bindgen::from_value::<TauriEvent<LogLine>>(event) {
                push_log(e.payload.text, e.payload.stderr);
            }
        });
        listen("log", &on_log).await;
        on_log.forget();
        let on_finished = Closure::<dyn FnMut(JsValue)>::new(move |event: JsValue| {
            if let Ok(e) = serde_wasm_bindgen::from_value::<TauriEvent<Finished>>(event) {
                let f = e.payload;
                let how = match (f.ok, f.code) {
                    (true, _) => "finished".to_string(),
                    (false, Some(code)) => format!("failed (exit code {code})"),
                    (false, None) => "stopped".to_string(),
                };
                push_log(format!("— {} {how}", f.kind), !f.ok);
                running.set(None);
                if f.kind == "build" {
                    check_built();
                }
            }
        });
        listen("job-finished", &on_finished).await;
        on_finished.forget();
    });

    let start = move |kind: &'static str| {
        running.set(Some(kind.to_string()));
        let dir = build_dir.get_untracked();
        let run_args = args.get_untracked();
        spawn_local(async move {
            let result = if kind == "build" {
                call::<_, ()>("build", &DirArgs { build_dir: dir }).await
            } else {
                call::<_, ()>("run_game", &RunArgs { build_dir: dir, args: run_args }).await
            };
            if let Err(e) = result {
                push_log(format!("— {kind}: {e}"), true);
                running.set(None);
            }
        });
    };
    let stop = move || {
        spawn_local(async move {
            if let Err(e) = call::<_, ()>("stop", &NoArgs {}).await {
                push_log(format!("— stop: {e}"), true);
            }
        });
    };

    // The log follows its newest line.
    let log_ref = NodeRef::<html::Pre>::new();
    Effect::new(move |_| {
        log.track();
        if let Some(pre) = log_ref.get() {
            pre.set_scroll_top(pre.scroll_height());
        }
    });

    let command = move || {
        // As run from the repository root.
        let dir = build_dir.get();
        let mut line = vec![shell_quote(&format!("{}/src/Main/bf2142", dir.trim().trim_end_matches('/')))];
        line.extend(args.get().iter().map(|a| shell_quote(a)));
        line.join(" ")
    };

    let flag_count = flags.len();
    let flags_on = {
        let flags = flags.clone();
        Memo::new(move |_| flags.iter().filter(|f| f.on.get()).count())
    };
    let set_all_flags = {
        let flags = flags.clone();
        move |on: bool| flags.iter().for_each(|f| f.on.set(on))
    };
    let reset_flags = {
        let flags = flags.clone();
        move || flags.iter().for_each(|f| f.on.set(f.flag.default))
    };

    // The enhanced options under their parts' headings, in the header's order.
    let mut categories: Vec<(String, Vec<FlagState>)> = Vec::new();
    for f in &flags {
        match categories.last_mut() {
            Some((name, list)) if *name == f.flag.category => list.push(f.clone()),
            _ => categories.push((f.flag.category.clone(), vec![f.clone()])),
        }
    }

    view! {
        <main>
            <h1>"BF2142 Launcher"</h1>
            <p class="muted">"Runs from " <code>{loaded.repo_root.clone()}</code></p>

            <section class="actions">
                <label>
                    "Build directory "
                    <input type="text" prop:value=move || build_dir.get()
                        on:input=move |ev| build_dir.set(event_target_value(&ev)) />
                </label>
                <span class="status">
                    {move || if built.get() { "Built" } else { "Not built" }}
                </span>
                <button on:click=move |_| start("build") disabled=move || running.get().is_some()>"Build"</button>
                <select
                    on:change=move |ev| start_with.set(event_target_value(&ev))
                    title="Without one of these the game opens no window">
                    <option value="menu" selected=move || start_with.get() == "menu">"Menu (--menu)"</option>
                    <option value="view" selected=move || start_with.get() == "view">"Level view (--view)"</option>
                    <option value="none" selected=move || start_with.get() == "none">"Only the options below"</option>
                </select>
                <button on:click=move |_| start("run")
                    disabled=move || running.get().is_some() || !built.get()>"Run"</button>
                <button on:click=move |_| stop() disabled=move || running.get().is_none()>"Stop"</button>
                <span class="status">{move || running.get().map(|k| format!("{k} running…")).unwrap_or_default()}</span>
                <button on:click=move |_| log.set(Vec::new())
                    hidden=move || log.with(Vec::is_empty)>"Clear output"</button>
            </section>

            // The build's (and the game's) output, following its newest line.
            <pre class="log" node_ref=log_ref hidden=move || log.with(Vec::is_empty)>
                {move || log.with(|l| l.iter().map(|(text, stderr)| {
                    let class = if *stderr { "stderr" } else { "" };
                    view! { <span class=class>{text.clone()} "\n"</span> }
                }).collect_view())}
            </pre>

            <label class="wide">
                <span>"BF2142 directory (" <code>"--game"</code> ")"</span>
                <span class="picker">
                    <input type="text" placeholder="the BF2142 install" prop:value=move || game_dir.get()
                        on:input=move |ev| game_dir.set(event_target_value(&ev)) />
                    <button on:click=move |ev| {
                        ev.prevent_default();
                        let start = game_dir.get_untracked();
                        spawn_local(async move {
                            if let Ok(Some(dir)) = call::<_, Option<String>>("pick_dir", &PickArgs { start }).await {
                                game_dir.set(dir);
                            }
                        });
                    }>"Choose…"</button>
                </span>
            </label>

            <MapPicker game_dir=game_dir start_with=start_with map=map />

            <fieldset>
                <legend>
                    <label>
                        <input type="checkbox" prop:checked=move || enhanced.get()
                            on:change=move |ev| enhanced.set(event_target_checked(&ev)) />
                        " Enhanced mode (" <code>"--enhanced"</code> ")"
                    </label>
                </legend>
                <details class:inactive=move || !enhanced.get()>
                    <summary>
                        "Enhanced options (" {move || flags_on.get()} " of " {flag_count} " on" {move || if enhanced.get() { "" } else { ", unused until Enhanced mode is ticked" }} ")"
                    </summary>
                    <p class="row">
                        <label>
                            <input type="checkbox"
                                prop:checked=move || flags_on.get() == flag_count
                                prop:indeterminate=move || { let n = flags_on.get(); n > 0 && n < flag_count }
                                prop:disabled=move || !enhanced.get()
                                on:change={
                                    let set_all_flags = set_all_flags.clone();
                                    move |_| set_all_flags(flags_on.get_untracked() != flag_count)
                                } />
                            " All"
                        </label>
                        <button on:click={
                            let reset_flags = reset_flags.clone();
                            move |_| reset_flags()
                        } disabled=move || !enhanced.get()>"Defaults"</button>
                    </p>
                    {categories
                        .into_iter()
                        .map(|(name, list)| view! {
                            <h4>{name}</h4>
                            <ul class="options">
                                {list
                                    .into_iter()
                                    .map(|f| {
                                        let on = f.on;
                                        let label = format!("enhanced.{}", f.flag.name);
                                        let default = if f.flag.default { "on" } else { "off" };
                                        view! {
                                            <li>
                                                <label>
                                                    <input type="checkbox" prop:checked=move || on.get()
                                                        prop:disabled=move || !enhanced.get()
                                                        on:change=move |ev| on.set(event_target_checked(&ev)) />
                                                    " " <code>{label}</code>
                                                    <small>" (default " {default} ")"</small>
                                                </label>
                                                <small class="description">{f.flag.description}</small>
                                            </li>
                                        }
                                    })
                                    .collect_view()}
                            </ul>
                        })
                        .collect_view()}
                </details>
            </fieldset>

            {groups
                .into_iter()
                .enumerate()
                .map(|(i, (name, list))| {
                    let count = {
                        let list = list.clone();
                        Memo::new(move |_| list.iter().filter(|o| o.on.get()).count())
                    };
                    view! {
                        <details open=i == 0>
                            <summary>{name} {move || match count.get() { 0 => String::new(), n => format!(" ({n} on)") }}</summary>
                            <ul class="options">
                                {list.into_iter().map(|o| view! { <OptionRow o=o /> }).collect_view()}
                            </ul>
                        </details>
                    }
                })
                .collect_view()}

            <label class="wide">
                "Extra arguments"
                <input type="text" placeholder="anything else, e.g. --console \"sv.timeLimit 600\""
                    prop:value=move || extra.get() on:input=move |ev| extra.set(event_target_value(&ev)) />
            </label>

            <h3 class="row">
                "Command"
                <button on:click=move |_| copy_text(&command())>"Copy"</button>
            </h3>
            <pre class="command">{command}</pre>

        </main>
    }
}

#[component]
fn OptionRow(o: OptState) -> impl IntoView {
    let on = o.on;
    let value = o.value;
    let input = o.opt.arg.clone().map(|placeholder| {
        view! {
            <input type="text" placeholder=placeholder prop:value=move || value.get()
                on:input=move |ev| {
                    let v = event_target_value(&ev);
                    if !v.trim().is_empty() {
                        on.set(true);
                    }
                    value.set(v);
                } />
        }
    });
    view! {
        <li>
            <label>
                <input type="checkbox" prop:checked=move || on.get()
                    on:change=move |ev| on.set(event_target_checked(&ev)) />
                " " <code>{o.opt.name.clone()}</code>
            </label>
            {input}
            <small class="description">{o.opt.description.clone()}</small>
        </li>
    }
}

fn copy_text(text: &str) {
    if let Some(window) = web_sys::window() {
        let _ = window.navigator().clipboard().write_text(text);
    }
}

// The levels of the BF2142 directory's mods: a filterable list, the chosen
// level's modes and sizes (from its server.zip and Info/<level>.desc), its
// picture, and the settings of a direct start.
#[component]
fn MapPicker(game_dir: RwSignal<String>, start_with: RwSignal<String>, map: MapChoice) -> impl IntoView {
    // None while reading.
    let list = RwSignal::new(None::<Result<LevelList, String>>);
    let reads = StoredValue::new(0u32);
    let reload = move || {
        let args = LevelArgs { game_dir: game_dir.get_untracked(), mod_name: map.mod_name.get_untracked() };
        let read = reads.get_value() + 1;
        reads.set_value(read);
        list.set(None);
        spawn_local(async move {
            let result = call::<_, LevelList>("list_levels", &args).await;
            // Only the newest read counts (the directory may change as it's typed).
            if reads.get_value() == read {
                list.set(Some(result));
            }
        });
    };
    Effect::new(move |_| {
        game_dir.track();
        map.mod_name.track();
        reload();
    });

    let listed = move || list.with(|l| l.as_ref().and_then(|r| r.as_ref().ok()).cloned());
    let selected = Memo::new(move |_| {
        let name = map.level.get();
        list.with(|l| match l {
            Some(Ok(list)) => list.levels.iter().find(|level| level.name == name).cloned(),
            _ => None,
        })
    });
    let visible = Memo::new(move |_| {
        let filter = map.filter.get().trim().to_lowercase();
        let mode = map.mode_filter.get();
        let navmesh_only = map.navmesh_only.get();
        list.with(|l| match l {
            Some(Ok(list)) => list
                .levels
                .iter()
                .filter(|level| {
                    (filter.is_empty()
                        || level.name.to_lowercase().contains(&filter)
                        || level.title.to_lowercase().contains(&filter))
                        && (mode.is_empty() || level.modes.iter().any(|m| m.mode.eq_ignore_ascii_case(&mode) && m.sizes.iter().any(|s| s.playable)))
                        && (!navmesh_only || level.navmesh)
                })
                .cloned()
                .collect::<Vec<_>>(),
            _ => Vec::new(),
        })
    });
    let total = Memo::new(move |_| listed().map_or(0, |l| l.levels.len()));
    let mods = Memo::new(move |_| listed().map(|l| l.mods).unwrap_or_default());
    let modes = Memo::new(move |_| listed().map(|l| l.modes).unwrap_or_default());

    // The picture for the chosen mode and size.
    let preview = RwSignal::new(None::<String>);
    let previews = StoredValue::new(0u32);
    Effect::new(move |_| {
        let args = PreviewArgs {
            game_dir: game_dir.get(),
            mod_name: map.mod_name.get(),
            level: map.level.get(),
            mode: map.game_mode.get(),
            size: map.map_size.get(),
        };
        let read = previews.get_value() + 1;
        previews.set_value(read);
        if args.level.is_empty() {
            preview.set(None);
            return;
        }
        spawn_local(async move {
            let picture = call::<_, Option<String>>("level_preview", &args).await.ok().flatten();
            if previews.get_value() == read {
                preview.set(picture);
            }
        });
    });

    let menu = move || start_with.get() == "menu";
    let choose = move |level: &Level| {
        map.choose(level);
        // A level chosen is a direct start: the menu would pick its own.
        if start_with.get_untracked() == "menu" {
            start_with.set("view".to_string());
        }
    };

    view! {
        <fieldset class="maps">
            <legend>"Map"</legend>
            <p class="row wrap">
                <label title="+modPath mods/<mod> (left out for bf2142)">
                    "Mod "
                    <select on:change=move |ev| map.mod_name.set(event_target_value(&ev))>
                        {move || {
                            let mut names = mods.get();
                            let current = map.mod_name.get();
                            if !names.contains(&current) {
                                names.insert(0, current.clone());
                            }
                            names
                                .into_iter()
                                .map(|name| {
                                    let chosen = name == current;
                                    view! { <option value=name.clone() selected=chosen>{name.clone()}</option> }
                                })
                                .collect_view()
                        }}
                    </select>
                </label>
                <input type="search" placeholder="Filter levels" prop:value=move || map.filter.get()
                    on:input=move |ev| map.filter.set(event_target_value(&ev)) />
                <select on:change=move |ev| map.mode_filter.set(event_target_value(&ev))
                    title="Only levels the game can load in this mode">
                    <option value="" selected=move || map.mode_filter.get().is_empty()>"Any mode"</option>
                    {move || modes.get().into_iter().map(|m| {
                        let value = m.mode.clone();
                        let chosen = move || map.mode_filter.get() == value;
                        view! { <option value=m.mode.clone() selected=chosen>{format!("{} ({})", m.title, m.mode)}</option> }
                    }).collect_view()}
                </select>
                <label title="Levels with AIPathFinding/Infantry.qtr: the game adds bots only there">
                    <input type="checkbox" prop:checked=move || map.navmesh_only.get()
                        on:change=move |ev| map.navmesh_only.set(event_target_checked(&ev)) />
                    " With a bot navmesh"
                </label>
                <button on:click=move |_| reload()>"Reload"</button>
                <span class="status">
                    {move || match list.with(|l| l.as_ref().map(Result::is_ok)) {
                        None => "Reading the levels…".to_string(),
                        Some(false) => String::new(),
                        Some(true) => format!("{} of {} levels", visible.with(Vec::len), total.get()),
                    }}
                </span>
            </p>
            {move || list.with(|l| match l {
                Some(Err(e)) => Some(view! { <p class="error">"Couldn't list the levels: " {e.clone()}</p> }),
                _ => None,
            })}
            <div class="map-picker">
                <ul class="levels">
                    <li class:chosen=move || map.level.get().is_empty() on:click=move |_| map.level.set(String::new())>
                        <span class="title">"No level"</span>
                        <small class="description">"the menu, or only the options below"</small>
                    </li>
                    {move || visible.get().into_iter().map(|level| {
                        let name = level.name.clone();
                        let chosen = move || map.level.get() == name;
                        let modes = level
                            .modes
                            .iter()
                            .filter(|m| m.sizes.iter().any(|s| s.playable))
                            .map(|m| m.mode.trim_start_matches("gpm_").to_string())
                            .collect::<Vec<_>>()
                            .join(" ");
                        let problems = level.problems.join("; ");
                        let unplayable = !level.playable();
                        let navmesh = level.navmesh;
                        let has_problems = !problems.is_empty();
                        let title = level.title.clone();
                        view! {
                            <li class:chosen=chosen class:unplayable=unplayable title=problems on:click=move |_| choose(&level)>
                                <span class="title">{title}</span>
                                {navmesh.then(|| view! { <span class="badge" title="Has an infantry navmesh (AIPathFinding/Infantry.qtr): bots play here">"nav"</span> })}
                                {has_problems.then(|| view! { <span class="badge warn">"!"</span> })}
                                <small class="description">{modes}</small>
                            </li>
                        }
                    }).collect_view()}
                </ul>
                <div class="level">{move || level_details(map, selected, list.with(Option::is_none), preview)}</div>
            </div>
            <ul class="options map-settings" class:inactive=move || map.level.get().is_empty() || menu()>
                <li>
                    <label>
                        <code>"--bots"</code>
                        <input type="number" min="0" max="64" prop:value=move || map.bots.get().to_string()
                            on:input=move |ev| map.bots.set(event_target_value(&ev).trim().parse().unwrap_or(0)) />
                    </label>
                    <small class="description">"AI players, alternately PAC and EU (0 for none). Only on levels with a navmesh (nav)."</small>
                </li>
                <li>
                    <label>
                        <code>"aiSettings.setBotSkill"</code>
                        <input type="number" min="0" max="1" step="0.05" placeholder="the AI's own"
                            prop:value=move || map.bot_skill.get()
                            on:input=move |ev| map.bot_skill.set(event_target_value(&ev)) />
                    </label>
                    <small class="description">"Bot skill, 0 to 1, passed as --console \"aiSettings.setBotSkill <n>\" (empty: AI/AIDefault.ai's). Only with bots."</small>
                </li>
                <li>
                    <label>
                        <code>"--time-limit"</code>
                        <input type="number" min="0" step="1" placeholder="minutes"
                            prop:value=move || map.time_limit.get()
                            on:input=move |ev| map.time_limit.set(event_target_value(&ev)) />
                    </label>
                    <small class="description">"The round's time limit in minutes, passed in seconds (empty for none)."</small>
                </li>
            </ul>
            {move || (menu() && !map.level.get().is_empty()).then(|| view! {
                <p class="hint">"With the menu, the menu picks the map: choose \"Level view\" or \"Only the options below\" to start in this level."</p>
            })}
        </fieldset>
    }
}

// The chosen level: its modes and sizes to choose from, its problems, the
// navmesh hint, its picture and briefing.
fn level_details(map: MapChoice, selected: Memo<Option<Level>>, reading: bool, preview: RwSignal<Option<String>>) -> AnyView {
    let name = map.level.get();
    if name.is_empty() {
        return view! {
            <p class="muted">"No level: Run opens the menu (or, with \"Only the options below\", whatever they ask for). Pick a level to start straight into it."</p>
        }
        .into_any();
    }
    let Some(level) = selected.get() else {
        return view! {
            <p class:muted=reading class:error=!reading>
                <code>{name}</code>
                {if reading { " (reading the levels…)" } else { " isn't one of this mod's levels" }}
            </p>
        }
        .into_any();
    };
    let modes = level.modes.clone();
    let chosen_mode = Memo::new(move |_| {
        let mode = map.game_mode.get();
        modes.iter().find(|m| m.mode.eq_ignore_ascii_case(&mode)).cloned()
    });
    view! {
        <h4>{level.title.clone()} " " <small><code>{level.name.clone()}</code></small></h4>
        {(!level.problems.is_empty()).then(|| view! { <p class="error">{level.problems.join("; ")}</p> })}
        <p class="choices">
            <span class="label">"Mode"</span>
            {level.modes.iter().map(|m| {
                let mode = m.clone();
                let id = m.mode.clone();
                let playable = m.sizes.iter().any(|s| s.playable);
                let tip = if playable { format!("+gameMode {}", m.mode) } else { format!("{}: server.zip has none of its sizes", m.mode) };
                view! {
                    <button class="chip" class:chosen=move || map.game_mode.get().eq_ignore_ascii_case(&id)
                        class:unplayable=!playable title=tip on:click=move |_| map.choose_mode(&mode)>{m.title.clone()}</button>
                }
            }).collect_view()}
        </p>
        <p class="choices">
            <span class="label">"Players"</span>
            {move || chosen_mode.get().map(|m| m.sizes.iter().map(|s| {
                let players = s.players;
                let tip = match (s.playable, s.listed) {
                    (true, true) => format!("+mapSize {players}"),
                    (true, false) => format!("+mapSize {players} (in server.zip, not in the .desc)"),
                    (false, _) => format!("in the .desc, but server.zip has no GameModes/{}/{players}/: the game can't load it", m.mode),
                };
                view! {
                    <button class="chip" class:chosen=move || map.map_size.get() == players
                        class:unplayable=!s.playable title=tip on:click=move |_| map.map_size.set(players)>{players}</button>
                }
            }).collect_view())}
        </p>
        {(!level.navmesh).then(|| view! {
            <p class="hint">"No infantry navmesh (AIPathFinding/Infantry.qtr): the game adds no bots on this level."</p>
        })}
        <div class="level-body">
            {move || preview.get().map(|src| view! { <img class="preview" src=src alt="" /> })}
            <p class="briefing">{level.description.clone()}</p>
        </div>
    }
    .into_any()
}

