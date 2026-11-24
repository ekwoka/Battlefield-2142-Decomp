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
                .map(|o| {
                    let kept = saved.options.get(&o.name).cloned().unwrap_or_else(|| SavedOpt { on: false, value: o.default_value.clone() });
                    OptState { opt: o.clone(), on: RwSignal::new(kept.on), value: RwSignal::new(kept.value) }
                })
                .collect();
            (g.name.clone(), options)
        })
        .collect();
    let options: Vec<OptState> = groups.iter().flat_map(|(_, o)| o.clone()).collect();

    // The command line, in the order: the game, the options, enhanced, extra.
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
        let mut line = vec!["bf2142".to_string()];
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

            <h3>"Command"</h3>
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

