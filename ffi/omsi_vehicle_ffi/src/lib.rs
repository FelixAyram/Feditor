//! C ABI over openOMSI's own vehicle simulation, so the Feditor editor drives exactly what
//! openOMSI drives: the bus scripts, the rigid body, the gearbox, the sound set and the
//! auto-start (Shift+U). openOMSI (https://github.com/openOMSI-org/openOMSI) is used unmodified
//! from `third_party/openOMSI`; GNU AGPL-3.0, see NOTICE.md.
//!
//! The editor keeps rendering, the camera, the map (ground heights + obstacle boxes) and the
//! keyboard; everything a bus does comes from here. See `include/omsi/vehicle/oo_vehicle.h`.

use std::ffi::{CStr, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use glam::{DVec2, DVec3, Vec3};
use omsi_sim::{Controls, KeyboardAxes, VehicleInstance, VehicleType};

// ---------------------------------------------------------------- logging into the editor's log

pub type OoLogFn = extern "C" fn(level: i32, msg: *const c_char);

static LOG_FN: OnceLock<OoLogFn> = OnceLock::new();

struct Forward;
impl log::Log for Forward {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Info
    }
    fn log(&self, r: &log::Record) {
        if !self.enabled(r.metadata()) {
            return;
        }
        if let Some(f) = LOG_FN.get() {
            let level = match r.level() {
                log::Level::Error => 2,
                log::Level::Warn => 1,
                _ => 0,
            };
            let text = format!("openOMSI: {}\0", r.args()).replace('\u{0}', " ");
            let mut bytes = text.into_bytes();
            if let Some(last) = bytes.last_mut() {
                *last = 0;
            }
            f(level, bytes.as_ptr() as *const c_char);
        }
    }
    fn flush(&self) {}
}
static FORWARD: Forward = Forward;

fn say(level: i32, msg: &str) {
    match level {
        2 => log::error!("{msg}"),
        1 => log::warn!("{msg}"),
        _ => log::info!("{msg}"),
    }
}

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(v) => v,
        Err(e) => {
            let what = e
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| e.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "panic".into());
            say(2, &format!("panic caught at the C boundary: {what}"));
            fallback
        }
    }
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

// ---------------------------------------------------------------- process-wide state

struct Global {
    root: PathBuf,
    audio: omsi_audio::AudioEngine,
    bindings: Vec<String>,
}
static GLOBAL: OnceLock<Global> = OnceLock::new();

/// The game's picture decoder for the simulation (as omsi-app registers it at start).
fn decode_picture(path: &Path) -> Result<omsi_sim::image_decode::DecodedImage, String> {
    omsi_texture::decode_file(path)
        .map(|i| omsi_sim::image_decode::DecodedImage { width: i.width, height: i.height, rgba: i.rgba, has_alpha: i.has_alpha })
        .map_err(|e| e.to_string())
}

fn default_output_device_name() -> String {
    use cpal::traits::{DeviceTrait, HostTrait};
    cpal::default_host()
        .default_output_device()
        .and_then(|d| d.name().ok())
        .unwrap_or_else(|| "(none)".into())
}

/// Install the log callback (may be null) and open the OMSI 2 content and the audio device.
/// Returns 1 on success. Safe to call more than once (later calls only report).
#[unsafe(no_mangle)]
pub extern "C" fn oo_init(omsi_root: *const c_char, log_fn: Option<OoLogFn>) -> i32 {
    if let Some(f) = log_fn {
        if LOG_FN.set(f).is_ok() {
            let _ = log::set_logger(&FORWARD);
            log::set_max_level(log::LevelFilter::Info);
        }
    }
    guard(0, || {
        if GLOBAL.get().is_some() {
            return 1;
        }
        let Some(root) = (unsafe { cstr(omsi_root) }) else {
            say(2, "oo_init: no OMSI 2 root");
            return 0;
        };
        let root = PathBuf::from(root);
        omsi_sim::image_decode::install(decode_picture);
        omsi_cfg::add_content_root(root.clone());
        let archives = root.join("Archives");
        if archives.is_dir() {
            omsi_cfg::vfs::mount_dir_zips(&archives);
        }
        let audio = omsi_audio::AudioEngine::new();
        say(
            0,
            &format!(
                "audio: {} on '{}' ({} Hz)",
                if audio.enabled { "device open" } else { "NO DEVICE (silent)" },
                default_output_device_name(),
                audio.sample_rate()
            ),
        );
        let kb = root.join("Inputs").join("keyboard.cfg");
        let bindings: Vec<String> = omsi_content::KeyboardCfg::load(&kb)
            .unwrap_or_default()
            .with_game_defaults()
            .vehicles
            .into_iter()
            .map(|b| b.action)
            .fold(Vec::new(), |mut acc, a| {
                if !acc.iter().any(|n: &String| n.eq_ignore_ascii_case(&a)) {
                    acc.push(a);
                }
                acc
            });
        say(0, &format!("keyboard.cfg: {} vehicle actions", bindings.len()));
        let _ = GLOBAL.set(Global { root, audio, bindings });
        1
    })
}

// ---------------------------------------------------------------- one bus

pub type OoGroundFn = extern "C" fn(user: *mut c_void, x: f64, y: f64, out_z: *mut f64) -> i32;

#[derive(Clone, Copy)]
struct GroundCb {
    f: OoGroundFn,
    user: *mut c_void,
}
// The editor calls oo_tick on its main thread and the callback answers there.
unsafe impl Send for GroundCb {}
unsafe impl Sync for GroundCb {}

pub struct OoBus {
    v: VehicleInstance,
    axes: KeyboardAxes,
    sounds: Option<omsi_audio::SoundSet>,
    startup: Option<omsi_sim::startup::StartUp>,
    startup_t: f32,
    held: Vec<String>,
    /// auto-start attempts in a row (retried while the engine does not really run)
    autostart_tries: u32,
    /// seconds since an auto-start ended, while we check the engine stays running
    verify_t: Option<f32>,
    /// the starter trigger the auto-start used (from its report), for re-cranking
    starter: Option<String>,
    /// a re-crank in progress: seconds the starter has been held
    crank_t: Option<f32>,
    /// analog pedals and steering (an autopilot / a controller), over the keyboard axes
    analog: Option<(f32, f32, f32)>,
}

/// An engine "running" that is really turning by itself: some mods (the Iguazu) raise
/// `engine_on` as soon as the starter turns, which openOMSI's auto-start takes for a start.
const RUNNING_RPM: f32 = 400.0;
/// Up to here the driver keeps the foot on the throttle while starting (idle governors of mod
/// engines only take over from ~500 rpm).
const ASSIST_RPM: f32 = 650.0;

/// "engine started (<trigger> held 1.4 s)" in openOMSI's auto-start report -> <trigger>.
fn starter_from_report(report: &[String]) -> Option<String> {
    report.iter().rev().find_map(|r| {
        let rest = r.strip_prefix("engine started (")?;
        let end = rest.find(" held")?;
        Some(rest[..end].to_string())
    })
}

/// Ready to drive once the engine runs: parking brake off and an automatic into D (with the foot on
/// the brake, as openOMSI's auto-start does it), whichever way the engine got started.
fn ensure_ready(b: &mut OoBus) -> Vec<String> {
    let mut done = Vec::new();
    if b.v.var("bremse_feststell").unwrap_or(0.0) > 0.5 {
        for t in ["parking_brake_release", "parking_brake_toggle"] {
            if b.v.ty.program.trigger(t).is_some() {
                b.v.trigger(t);
                b.v.trigger(&format!("{t}_off"));
                done.push(format!("parking brake released ({t})"));
                break;
            }
        }
    }
    if b.v.ty.program.trigger("automatic_D").is_some() && b.v.var("antrieb_getr_gangvorwahl").is_none_or(|g| g < 3.5) {
        let brake = b.v.var("Brake");
        b.v.set_var("Brake", 1.0);
        if b.v.ty.program.trigger("automatic_N").is_some() && b.v.var("cockpit_gangN").is_some_and(|n| n < 0.5) {
            b.v.trigger("automatic_N");
            b.v.trigger("automatic_N_off");
        }
        b.v.trigger("automatic_D");
        b.v.trigger("automatic_D_off");
        if let Some(x) = brake {
            b.v.set_var("Brake", x);
        }
        done.push("gear D".to_string());
    }
    done
}

fn begin_startup(b: &mut OoBus) {
    let Some(g) = global() else { return };
    let s = omsi_sim::startup::StartUp::new(&b.v, &g.bindings);
    say(
        0,
        &format!(
            "auto-start{}: {}",
            if b.autostart_tries > 0 { format!(" (try {})", b.autostart_tries + 1) } else { String::new() },
            if s.shutting_down() { "switching the vehicle off" } else { "putting the vehicle into service" }
        ),
    );
    b.startup = Some(s);
    b.startup_t = 0.0;
    b.verify_t = None;
}

mod traffic;

/// Copy `s` into the C buffer (NUL-terminated, cut to fit); returns the length written.
fn write_cstr(s: &str, buf: *mut c_char, cap: usize) -> usize {
    if buf.is_null() || cap == 0 {
        return 0;
    }
    let bytes = s.as_bytes();
    let n = bytes.len().min(cap - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
    n
}

fn global() -> Option<&'static Global> {
    GLOBAL.get()
}

/// Load `bus_path` and put it at (x, y, z) heading `heading_deg` (clockwise from north, the
/// editor's convention). Null on failure (the reason goes to the log).
#[unsafe(no_mangle)]
pub extern "C" fn oo_spawn(bus_path: *const c_char, x: f64, y: f64, z: f64, heading_deg: f64) -> *mut OoBus {
    guard(std::ptr::null_mut(), || {
        let Some(g) = global() else {
            say(2, "oo_spawn before oo_init");
            return std::ptr::null_mut();
        };
        let Some(path) = (unsafe { cstr(bus_path) }) else { return std::ptr::null_mut() };
        let p = Path::new(path);
        let full = if p.is_absolute() { p.to_path_buf() } else { omsi_cfg::resolve_path(&g.root, path) };
        let vt = match VehicleType::load(&g.root, &full) {
            Ok(t) => Arc::new(t),
            Err(e) => {
                say(2, &format!("cannot load {path}: {e}"));
                return std::ptr::null_mut();
            }
        };
        let mut v = VehicleInstance::new(vt.clone(), omsi_sim::VehicleHost::new(Default::default()));
        v.position = DVec3::new(x, y, z);
        v.heading = heading_deg;
        // its paint: OMSI's vehicle dialog always has a repaint picked - the first of the `[CTC]` folders'
        // `.cti` items - and sets `Colorscheme` and the item's `[setvar]`s before `{init}` (0x70a174).
        // Without one the AGG300 showed its CTC placeholders (`VAN_A_trans.tga`: alpha 0, its windows gone).
        let scheme = if vt.paint_schemes.is_empty() { None } else { Some(0) };
        v.host.paint_scheme = Some(scheme);
        v.apply_paint_vars(scheme);
        v.enable_rigid_body();
        attach_coupled_parts(&g.root, &mut v);
        let mut sounds = None;
        if let Some(rel) = &vt.def.sound {
            let path = omsi_cfg::resolve_path(vt.def.dir(), rel);
            match omsi_vehicle::SoundCfg::load(&path) {
                Ok(cfg) => {
                    let cfg = cfg.chosen_for(&v.number());
                    let dir = path.parent().map(|p| p.to_path_buf()).unwrap_or_default();
                    say(0, &format!("sound config {}: {} sounds", path.display(), cfg.sounds.len()));
                    let ss = omsi_audio::SoundSet::new(&g.audio, &cfg, &dir);
                    v.host.snapshot_triggers = ss.curve_triggers().into_iter().collect();
                    sounds = Some(ss);
                }
                Err(e) => say(1, &format!("sound config {}: {e}", path.display())),
            }
        }
        say(0, &format!("spawned {} at ({x:.1}, {y:.1}, {z:.2}) heading {heading_deg:.1}", vt.def.path.display()));
        Box::into_raw(Box::new(OoBus {
            v,
            axes: KeyboardAxes::default(),
            sounds,
            startup: None,
            startup_t: 0.0,
            held: Vec::new(),
            autostart_tries: 0,
            verify_t: None,
            starter: None,
            crank_t: None,
            analog: None,
        }))
    })
}

/// The `[couple_back]` chain behind `v` (articulated rear sections, trailers), as openOMSI's
/// `spawn::load_coupled_parts`: each part from the folder of the one before it, turned round when
/// the file says so, the chain closed where a part comes back; towed by openOMSI's trailer
/// kinematics, its meshes animated from the front section's variables.
fn attach_coupled_parts(root: &Path, v: &mut VehicleInstance) {
    use omsi_vehicle::vehicle::{MAX_COUPLED_PARTS, chain_has_part, next_coupled};
    let mut lead = v.ty.clone();
    let mut lead_rev = false;
    let mut seen = vec![lead.def.path.clone()];
    for _ in 0..MAX_COUPLED_PARTS {
        let Some((path, reversed)) = next_coupled(&lead.def, lead_rev, true) else { break };
        if chain_has_part(&seen, &path) {
            break;
        }
        match VehicleType::load(root, &path) {
            Ok(t) => {
                let t = Arc::new(t);
                say(0, &format!("coupled behind {}: {} ({} meshes{})", lead.def.path.display(), path.display(), t.meshes.len(), if reversed { ", reversed" } else { "" }));
                v.attach_trailer_ex(t.clone(), reversed);
                seen.push(path);
                lead = t;
                lead_rev = reversed;
            }
            Err(e) => {
                say(1, &format!("coupled part {}: {e}", path.display()));
                break;
            }
        }
    }
}

/// Mesh `i` of the whole train: (None = the front section | Some(part), mesh of that part).
fn train_mesh(b: &OoBus, i: usize) -> Option<(Option<usize>, usize)> {
    let n0 = b.v.ty.meshes.len();
    if i < n0 {
        return Some((None, i));
    }
    let mut k = i - n0;
    for (ti, t) in b.v.trailers.iter().enumerate() {
        let n = t.ty.meshes.len();
        if k < n {
            return Some((Some(ti), k));
        }
        k -= n;
    }
    None
}

/// Number of sections coupled behind the bus (articulated rear sections, trailers).
#[unsafe(no_mangle)]
pub extern "C" fn oo_section_count(bus: *mut OoBus) -> i32 {
    with_bus(bus, 0, |b| b.v.trailers.len() as i32)
}

/// Section `s` behind the bus: its .bus file (UTF-8) and its meshes' range in the train's mesh
/// indices (oo_mesh_*). Returns 1, 0 when out of range.
#[unsafe(no_mangle)]
pub extern "C" fn oo_section_info(bus: *mut OoBus, s: i32, buf: *mut c_char, cap: usize, first: *mut i32, count: *mut i32) -> i32 {
    with_bus(bus, 0, |b| {
        if s < 0 || s as usize >= b.v.trailers.len() {
            return 0;
        }
        let mut f = b.v.ty.meshes.len();
        for t in b.v.trailers.iter().take(s as usize) {
            f += t.ty.meshes.len();
        }
        let t = &b.v.trailers[s as usize];
        if !buf.is_null() && cap > 0 {
            let p = t.ty.def.path.to_string_lossy();
            let bytes = p.as_bytes();
            let n = bytes.len().min(cap - 1);
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
                *buf.add(n) = 0;
            }
        }
        if !first.is_null() {
            unsafe { *first = f as i32 };
        }
        if !count.is_null() {
            unsafe { *count = t.ty.meshes.len() as i32 };
        }
        1
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_despawn(bus: *mut OoBus) {
    if bus.is_null() {
        return;
    }
    guard((), || drop(unsafe { Box::from_raw(bus) }));
}

fn with_bus<T>(bus: *mut OoBus, fallback: T, f: impl FnOnce(&mut OoBus) -> T) -> T {
    if bus.is_null() {
        return fallback;
    }
    guard(fallback, || f(unsafe { &mut *bus }))
}

/// Ground height under (x, y) from the editor (terrain, roads, surface objects).
#[unsafe(no_mangle)]
pub extern "C" fn oo_set_ground(bus: *mut OoBus, f: Option<OoGroundFn>, user: *mut c_void) {
    with_bus(bus, (), |b| {
        b.v.ground = f.map(|f| {
            let cb = GroundCb { f, user };
            Arc::new(move |x: f64, y: f64| {
                let cb = cb;
                let mut z = 0.0f64;
                if (cb.f)(cb.user, x, y, &mut z) != 0 { Some(z) } else { None }
            }) as Arc<dyn Fn(f64, f64) -> Option<f64> + Send + Sync>
        });
    })
}

/// One obstacle box in the world: centre, half extents (x across, y along), heading
/// (radians, clockwise from north), height range, map object id.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OoBox {
    pub cx: f64,
    pub cy: f64,
    pub half_x: f64,
    pub half_y: f64,
    pub heading: f64,
    pub z0: f64,
    pub z1: f64,
    pub id: i64,
    pub pole_mass_t: f32,
    pub pole_load_kn: f32,
}

/// Replace the obstacles the bus collides with (the editor sends the ones near the bus).
#[unsafe(no_mangle)]
pub extern "C" fn oo_set_obstacles(bus: *mut OoBus, boxes: *const OoBox, n: usize) {
    with_bus(bus, (), |b| {
        let mut world = omsi_sim::collision::CollisionWorld::default();
        if !boxes.is_null() {
            for o in unsafe { std::slice::from_raw_parts(boxes, n) } {
                world.add(omsi_sim::collision::Obb {
                    center: DVec2::new(o.cx, o.cy),
                    half: DVec2::new(o.half_x, o.half_y),
                    heading: o.heading,
                    z0: o.z0,
                    z1: o.z1,
                    velocity: DVec2::ZERO,
                    mass: 0.0,
                    pole: (o.pole_mass_t > 0.0).then_some((o.pole_mass_t, o.pole_load_kn)),
                    id: o.id,
                });
            }
        }
        b.v.collision = Some(Arc::new(world));
        b.v.wheel_walls = true;
    })
}

/// A keyboard.cfg action pressed or released: pedals and steering go to openOMSI's keyboard
/// axes, everything else to the scripts (the `_off` trigger on release), as openOMSI's player.
#[unsafe(no_mangle)]
pub extern "C" fn oo_key(bus: *mut OoBus, action: *const c_char, pressed: i32) {
    let Some(name) = (unsafe { cstr(action) }) else { return };
    with_bus(bus, (), |b| {
        let pressed = pressed != 0;
        if let Some(a) = omsi_sim::engine_action(name) {
            b.axes.set(a, pressed);
            return;
        }
        if pressed {
            if !b.held.iter().any(|h| h.eq_ignore_ascii_case(name)) {
                b.held.push(name.to_string());
            }
            if !b.v.trigger(name) {
                say(0, &format!("action {name}: no such trigger in this bus"));
            }
        } else {
            b.held.retain(|h| !h.eq_ignore_ascii_case(name));
            b.v.trigger(&format!("{name}_off"));
        }
    })
}

/// openOMSI's auto-start (Shift+U): electrics on, starter held until the engine runs, gear
/// selected. Pressing it again on a running bus switches it off, as in openOMSI.
#[unsafe(no_mangle)]
pub extern "C" fn oo_autostart(bus: *mut OoBus) {
    with_bus(bus, (), |b| {
        b.autostart_tries = 0;
        begin_startup(b);
    })
}

/// One frame: auto-start, keyboard axes, controls, scripts + physics, then the sound set with
/// the camera as the listener (forward = view direction, world axes).
#[unsafe(no_mangle)]
pub extern "C" fn oo_tick(bus: *mut OoBus, dt: f32, lx: f32, ly: f32, lz: f32, fx: f32, fy: f32, fz: f32) {
    with_bus(bus, (), |b| {
        let dt = dt.clamp(0.0, 0.1);
        // Driver's foot during the auto-start: many mod engines (the Iguazu's ISBe: no injection under
        // 500 rpm without the pedal) only catch with throttle while the starter turns. openOMSI's
        // auto-start cranks without it; we hold half throttle while the starter is engaged and the
        // engine has not caught.
        let mut assist_throttle = 0.0f32;
        if let (Some(mut s), Some(g)) = (b.startup.take(), global()) {
            b.startup_t += dt;
            let rpm = omsi_sim::startup::engine_rpm(&b.v).unwrap_or(0.0);
            if !s.shutting_down() && omsi_sim::startup::power_on(&b.v) && rpm < ASSIST_RPM {
                assist_throttle = 0.5;
            }
            if s.tick(&mut b.v, &g.bindings, dt) && b.startup_t < 45.0 {
                b.startup = Some(s);
            } else {
                let running = omsi_sim::startup::engine_running(&b.v);
                // ready to drive: let go of the parking brake (the scripts' own release trigger)
                if running && !s.shutting_down() && b.v.var("bremse_feststell").unwrap_or(0.0) > 0.5 {
                    for t in ["parking_brake_release", "parking_brake_toggle"] {
                        if b.v.ty.program.trigger(t).is_some() {
                            b.v.trigger(t);
                            b.v.trigger(&format!("{t}_off"));
                            s.report.push(format!("parking brake released ({t})"));
                            break;
                        }
                    }
                }
                say(0, &format!("auto-start finished: {} (engine running: {running})", s.report.join(", ")));
                if !s.shutting_down() {
                    b.verify_t = Some(0.0);
                    if let Some(st) = starter_from_report(&s.report) {
                        b.starter = Some(st);
                    }
                }
            }
        } else if let Some(t) = b.crank_t.as_mut() {
            // re-crank: the auto-start's own starter held, throttle on, until the engine runs
            *t += dt;
            let rpm = omsi_sim::startup::engine_rpm(&b.v).unwrap_or(0.0);
            assist_throttle = 0.6;
            if rpm >= ASSIST_RPM || *t > 8.0 {
                if let Some(st) = b.starter.clone() {
                    b.v.trigger(&format!("{st}_off"));
                }
                say(0, &format!("re-crank released at {rpm:.0} rpm"));
                b.crank_t = None;
                b.verify_t = Some(0.0);
            }
        } else if let Some(t) = b.verify_t.as_mut() {
            // a start that does not hold: the engine below RUNNING_RPM a few seconds after the
            // auto-start is cranked again (with the foot on the throttle), up to three times
            *t += dt;
            let rpm = omsi_sim::startup::engine_rpm(&b.v).unwrap_or(0.0);
            if omsi_sim::startup::power_on(&b.v) && rpm < ASSIST_RPM {
                assist_throttle = 0.5;
            }
            if rpm >= RUNNING_RPM {
                if *t > 3.0 {
                    b.verify_t = None;
                    let done = ensure_ready(b);
                    say(0, &format!("engine running ({rpm:.0} rpm){}", if done.is_empty() { String::new() } else { format!(": {}", done.join(", ")) }));
                }
            } else if *t > 2.0 {
                b.verify_t = None;
                if b.autostart_tries < 3 {
                    b.autostart_tries += 1;
                    match b.starter.clone() {
                        Some(st) => {
                            say(1, &format!("engine at {rpm:.0} rpm after the auto-start: cranking again ({st})"));
                            b.v.trigger(&st);
                            b.crank_t = Some(0.0);
                        }
                        None => {
                            say(1, &format!("engine at {rpm:.0} rpm after the auto-start: auto-start again"));
                            begin_startup(b);
                        }
                    }
                } else {
                    say(1, "engine would not start after 4 tries");
                }
            }
        }
        b.axes.speed_kmh = b.v.physics.velocity_kmh();
        b.axes.lock_curvature = b.v.ty.def.inv_min_turn_radius;
        b.axes.update(dt);
        let (thr, brk, steer) = match b.analog {
            Some((t, br, st)) => (t.max(b.axes.throttle), br.max(b.axes.brake), st),
            None => (b.axes.throttle, b.axes.brake, b.axes.steering),
        };
        b.v.set_controls(Controls { throttle: thr.max(assist_throttle), brake: brk, clutch: b.axes.clutch, steering: steer });
        b.v.update(dt);
        if let Some(g) = global() {
            g.audio.follow_device();
            let fwd = Vec3::new(fx, fy, fz).normalize_or(Vec3::Y);
            let right = fwd.cross(Vec3::Z).normalize_or(Vec3::X);
            g.audio.set_listener(omsi_audio::mixer::Listener { position: Vec3::new(lx, ly, lz), forward: fwd, right, ..Default::default() });
            let fired: Vec<String> = std::mem::take(&mut b.v.host.fired_triggers);
            let fired_vars: Vec<(String, Vec<f32>)> = std::mem::take(&mut b.v.host.fired_trigger_vars);
            if let Some(ss) = b.sounds.as_mut() {
                let xf = b.v.world_transform();
                let v = &b.v;
                let at_fire = |t: &str, n: &str| -> Option<f32> {
                    let vals = &fired_vars.iter().rev().find(|(k, _)| k.eq_ignore_ascii_case(t))?.1;
                    v.var_slot(n).and_then(|i| vals.get(i).copied())
                };
                ss.update_fired(&g.audio, &|n| v.var(n), &xf, &fired, &at_fire);
            }
        }
    })
}

/// The body's world matrix (model frame → world), 16 floats in glam column order — which is
/// DirectXMath's row-vector matrix in row-major order, ready for XMLoadFloat4x4.
#[unsafe(no_mangle)]
pub extern "C" fn oo_body_matrix(bus: *mut OoBus, out16: *mut f32) -> i32 {
    if out16.is_null() {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let rot = b.v.body_rotation();
        let m = glam::Mat4::from_translation(b.v.position.as_vec3()) * rot;
        unsafe { std::ptr::copy_nonoverlapping(m.to_cols_array().as_ptr(), out16, 16) };
        1
    })
}

/// Position (model origin) and heading / pitch / bank in degrees.
#[unsafe(no_mangle)]
pub extern "C" fn oo_pose(bus: *mut OoBus, out: *mut f64) -> i32 {
    if out.is_null() {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let p = b.v.position;
        let vals = [p.x, p.y, p.z, b.v.heading, b.v.pitch as f64, b.v.bank as f64];
        unsafe { std::ptr::copy_nonoverlapping(vals.as_ptr(), out, 6) };
        1
    })
}

/// Slot of script variable `name` (case-insensitive), -1 when the bus has none.
#[unsafe(no_mangle)]
pub extern "C" fn oo_var_slot(bus: *mut OoBus, name: *const c_char) -> i32 {
    let Some(name) = (unsafe { cstr(name) }) else { return -1 };
    with_bus(bus, -1, |b| b.v.var_slot(name).map(|i| i as i32).unwrap_or(-1))
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_var_at(bus: *mut OoBus, slot: i32) -> f32 {
    with_bus(bus, 0.0, |b| if slot < 0 { 0.0 } else { b.v.state.vars.get(slot as usize).copied().unwrap_or(0.0) })
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_var(bus: *mut OoBus, name: *const c_char, fallback: f32) -> f32 {
    let Some(name) = (unsafe { cstr(name) }) else { return fallback };
    with_bus(bus, fallback, |b| b.v.var(name).unwrap_or(fallback))
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_set_var(bus: *mut OoBus, name: *const c_char, value: f32) -> i32 {
    let Some(name) = (unsafe { cstr(name) }) else { return 0 };
    with_bus(bus, 0, |b| b.v.set_var(name, value) as i32)
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct OoStatus {
    pub speed_kmh: f32,
    pub engine_rpm: f32,
    pub engine_running: i32,
    pub power_on: i32,
    pub autostart_running: i32,
    pub voices: i32,
    pub audio_ok: i32,
    pub throttle: f32,
    pub brake: f32,
    pub steering: f32,
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_status(bus: *mut OoBus, out: *mut OoStatus) -> i32 {
    if out.is_null() {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let g = global();
        let s = OoStatus {
            speed_kmh: b.v.physics.velocity_kmh(),
            engine_rpm: omsi_sim::startup::engine_rpm(&b.v).unwrap_or(0.0),
            engine_running: omsi_sim::startup::engine_running(&b.v) as i32,
            power_on: omsi_sim::startup::power_on(&b.v) as i32,
            autostart_running: b.startup.is_some() as i32,
            voices: g.map(|g| g.audio.voice_count() as i32).unwrap_or(0),
            audio_ok: g.map(|g| g.audio.enabled as i32).unwrap_or(0),
            throttle: b.axes.throttle,
            brake: b.axes.brake,
            steering: b.axes.steering,
        };
        unsafe { *out = s };
        1
    })
}

static TEST_LOCK: Mutex<()> = Mutex::new(());
#[doc(hidden)]
pub fn test_lock() -> std::sync::MutexGuard<'static, ()> {
    TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}


// ---------------------------------------------------------------- meshes as openOMSI poses them

/// Number of meshes openOMSI animates for this bus (LOD 0, model.cfg order).
#[unsafe(no_mangle)]
pub extern "C" fn oo_mesh_count(bus: *mut OoBus) -> i32 {
    // the front section's meshes, then each coupled section's (see oo_section_info)
    with_bus(bus, 0, |b| (b.v.ty.meshes.len() + b.v.trailers.iter().map(|t| t.ty.meshes.len()).sum::<usize>()) as i32)
}

/// The paint of the player's train mesh i: its scheme's texture swaps as "<default texture, lower case>\t<file>\n"
/// lines (openOMSI `scheme_substitutions`). A coupled section takes the scheme of the same name as the front's
/// (the AGG300's three `.cti` folders list the same adverts), else the same index. Returns the bytes written.
#[unsafe(no_mangle)]
pub extern "C" fn oo_bus_paint(bus: *mut OoBus, i: i32, buf: *mut c_char, cap: usize) -> usize {
    with_bus(bus, 0, |b| {
        let Some((part, _)) = train_mesh(b, i.max(0) as usize) else { return 0 };
        let front = b.v.host.paint_scheme.flatten();
        let ty = match part {
            None => &b.v.ty,
            Some(t) => &b.v.trailers[t].ty,
        };
        let scheme = match (part, front) {
            (None, s) => s,
            (Some(_), Some(s)) => b
                .v
                .ty
                .paint_schemes
                .get(s)
                .and_then(|p| ty.paint_schemes.iter().position(|q| q.name.eq_ignore_ascii_case(&p.name)))
                .or((s < ty.paint_schemes.len()).then_some(s)),
            _ => None,
        };
        let Some(s) = scheme else { return 0 };
        let mut out = String::new();
        for (from, to) in ty.scheme_substitutions(s, omsi_texture::find_texture).0 {
            out.push_str(&from);
            out.push('\t');
            out.push_str(&to);
            out.push('\n');
        }
        write_cstr(&out, buf, cap)
    })
}

/// The text a `[texttexture]` / `[texttexture_enh]` field shows now (UTF-8): a script string variable of the
/// player's bus, or the number of a built-in string (openOMSI `text_texture_string`). Returns the bytes written.
#[unsafe(no_mangle)]
pub extern "C" fn oo_bus_texttex_string(bus: *mut OoBus, field: *const c_char, buf: *mut c_char, cap: usize) -> usize {
    if field.is_null() {
        return 0;
    }
    let field = unsafe { CStr::from_ptr(field) }.to_string_lossy().into_owned();
    with_bus(bus, 0, |b| write_cstr(&b.v.text_texture_string(field.trim()), buf, cap))
}

/// Mesh i's .o3d path (UTF-8, NUL-terminated); returns the length, 0 when out of range.
#[unsafe(no_mangle)]
pub extern "C" fn oo_mesh_file(bus: *mut OoBus, i: i32, buf: *mut c_char, cap: usize) -> usize {
    if buf.is_null() || cap == 0 {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let Some((part, k)) = train_mesh(b, i.max(0) as usize) else { return 0 };
        let ty = match part {
            None => &b.v.ty,
            Some(t) => &b.v.trailers[t].ty,
        };
        let Some(m) = ty.meshes.get(k) else { return 0 };
        let s = m.file.to_string_lossy();
        let bytes = s.as_bytes();
        let n = bytes.len().min(cap - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
            *buf.add(n) = 0;
        }
        n
    })
}

/// Mesh i in the world (body pose x its animation; shadow blobs laid on the wheels' plane), as a
/// DirectXMath row-vector matrix in row-major order (glam's column order).
#[unsafe(no_mangle)]
pub extern "C" fn oo_mesh_world(bus: *mut OoBus, i: i32, out16: *mut f32) -> i32 {
    if out16.is_null() || i < 0 {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let Some((part, k)) = train_mesh(b, i as usize) else { return 0 };
        let m = match part {
            None => glam::Mat4::from_translation(b.v.position.as_vec3()) * b.v.mesh_local_transform(k),
            Some(t) => {
                let tp = &b.v.trailers[t];
                glam::Mat4::from_translation(tp.position.as_vec3()) * tp.mesh_local_transform(k)
            }
        };
        unsafe { std::ptr::copy_nonoverlapping(m.to_cols_array().as_ptr(), out16, 16) };
        1
    })
}

/// Whether mesh i is shown ([visible] / [visible_range] conditions on the script variables).
#[unsafe(no_mangle)]
pub extern "C" fn oo_mesh_visible(bus: *mut OoBus, i: i32) -> i32 {
    with_bus(bus, 1, |b| {
        let Some((part, k)) = train_mesh(b, i.max(0) as usize) else { return 0 };
        let v = &b.v;
        let ty = match part {
            None => &v.ty,
            Some(t) => &v.trailers[t].ty,
        };
        let Some(m) = ty.meshes.get(k) else { return 0 };
        // (a coupled section's meshes go by the front section's variables, as it animates)
        ty.model.meshes.get(m.def_index).and_then(|d| d.visible_by(|n| v.var(n))).unwrap_or(true) as i32
    })
}

/// Analog controls over the keys (throttle 0..1, brake 0..1, steering -1 left .. +1 right);
/// active = 0 hands control back to the keyboard axes.
#[unsafe(no_mangle)]
pub extern "C" fn oo_set_analog(bus: *mut OoBus, active: i32, throttle: f32, brake: f32, steering: f32) {
    with_bus(bus, (), |b| {
        b.analog = (active != 0).then_some((throttle.clamp(0.0, 1.0), brake.clamp(0.0, 1.0), steering.clamp(-1.0, 1.0)));
    })
}

/// Full lock curvature (1/m) of the bus, for an autopilot's steering.
#[unsafe(no_mangle)]
pub extern "C" fn oo_inv_min_turn_radius(bus: *mut OoBus) -> f32 {
    with_bus(bus, 0.1, |b| b.v.ty.def.inv_min_turn_radius)
}

// ---------------------------------------------------------------- audio probe (loopback)

/// Record what reaches every output device (WASAPI loopback) for `seconds` on a background
/// thread: logs each device's level once a second and writes `<wav_dir>/oo_loopback_<n>.wav`
/// (16-bit) per device. Proves whether the sound leaves the program and where it goes.
#[unsafe(no_mangle)]
pub extern "C" fn oo_audio_probe(seconds: f32, wav_dir: *const c_char) -> i32 {
    let dir = unsafe { cstr(wav_dir) }.map(PathBuf::from);
    guard(0, || {
        std::thread::spawn(move || {
            if catch_unwind(AssertUnwindSafe(|| loopback_all(seconds, dir))).is_err() {
                say(2, "audio probe panicked");
            }
        });
        1
    })
}

struct Rec {
    name: String,
    channels: usize,
    rate: u32,
    buf: Arc<Mutex<Vec<f32>>>,
    _stream: cpal::Stream,
}

fn loopback_all(seconds: f32, dir: Option<PathBuf>) {
    use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
    let host = cpal::default_host();
    let default_name = host.default_output_device().and_then(|d| d.name().ok()).unwrap_or_default();
    let devices: Vec<cpal::Device> = match host.output_devices() {
        Ok(d) => d.collect(),
        Err(e) => {
            say(2, &format!("audio probe: no output devices: {e}"));
            return;
        }
    };
    let mut recs: Vec<Rec> = Vec::new();
    for (n, d) in devices.into_iter().enumerate() {
        let name = d.name().unwrap_or_else(|_| format!("device {n}"));
        let Ok(cfg) = d.default_output_config() else { continue };
        let channels = cfg.channels() as usize;
        let rate = cfg.sample_rate().0;
        let buf = Arc::new(Mutex::new(Vec::<f32>::new()));
        let b2 = buf.clone();
        let format = cfg.sample_format();
        let stream = match format {
            cpal::SampleFormat::F32 => d.build_input_stream(
                &cfg.into(),
                move |data: &[f32], _: &cpal::InputCallbackInfo| {
                    b2.lock().unwrap_or_else(|e| e.into_inner()).extend_from_slice(data)
                },
                |e| say(1, &format!("audio probe stream error: {e}")),
                None,
            ),
            cpal::SampleFormat::I16 => d.build_input_stream(
                &cfg.into(),
                move |data: &[i16], _: &cpal::InputCallbackInfo| {
                    b2.lock().unwrap_or_else(|e| e.into_inner()).extend(data.iter().map(|&x| x as f32 / 32768.0))
                },
                |e| say(1, &format!("audio probe stream error: {e}")),
                None,
            ),
            f => {
                say(1, &format!("audio probe: {name}: sample format {f:?} not handled"));
                continue;
            }
        };
        match stream {
            Ok(s) => {
                if s.play().is_ok() {
                    let tag = if name == default_name { " (default)" } else { "" };
                    say(0, &format!("audio probe: listening to {name}{tag}"));
                    recs.push(Rec { name, channels, rate, buf, _stream: s });
                }
            }
            Err(e) => say(1, &format!("audio probe: cannot loop back {name}: {e}")),
        }
    }
    let mut kept: Vec<Vec<f32>> = vec![Vec::new(); recs.len()];
    let secs = seconds.clamp(1.0, 120.0) as usize;
    for s in 0..secs {
        std::thread::sleep(std::time::Duration::from_secs(1));
        let mut line = format!("audio probe t={}s:", s + 1);
        for (k, r) in recs.iter().enumerate() {
            let chunk: Vec<f32> = std::mem::take(&mut *r.buf.lock().unwrap_or_else(|e| e.into_inner()));
            let rms = if chunk.is_empty() { 0.0 } else { (chunk.iter().map(|x| x * x).sum::<f32>() / chunk.len() as f32).sqrt() };
            let peak = chunk.iter().fold(0f32, |m, x| m.max(x.abs()));
            line += &format!(" [{} rms={rms:.4} peak={peak:.3} n={}]", r.name, chunk.len());
            kept[k].extend_from_slice(&chunk);
        }
        say(0, &line);
    }
    if let Some(dir) = dir {
        for (k, r) in recs.iter().enumerate() {
            let path = dir.join(format!("oo_loopback_{k}.wav"));
            if write_wav(&path, &kept[k], r.channels, r.rate).is_ok() {
                say(0, &format!("audio probe: wrote {} ({})", path.display(), r.name));
            }
        }
    }
}

fn write_wav(path: &Path, samples: &[f32], channels: usize, rate: u32) -> std::io::Result<()> {
    use std::io::Write;
    let ch = channels.max(1) as u16;
    let data_len = (samples.len() * 2) as u32;
    let mut f = std::io::BufWriter::new(std::fs::File::create(path)?);
    f.write_all(b"RIFF")?;
    f.write_all(&(36 + data_len).to_le_bytes())?;
    f.write_all(b"WAVEfmt ")?;
    f.write_all(&16u32.to_le_bytes())?;
    f.write_all(&1u16.to_le_bytes())?;
    f.write_all(&ch.to_le_bytes())?;
    f.write_all(&rate.to_le_bytes())?;
    f.write_all(&(rate * ch as u32 * 2).to_le_bytes())?;
    f.write_all(&(ch * 2).to_le_bytes())?;
    f.write_all(&16u16.to_le_bytes())?;
    f.write_all(b"data")?;
    f.write_all(&data_len.to_le_bytes())?;
    for &x in samples {
        f.write_all(&((x.clamp(-1.0, 1.0) * 32767.0) as i16).to_le_bytes())?;
    }
    Ok(())
}


/// Log openOMSI's own report of the bus's sound set: every entry heard (level, pitch), ready but
/// silent, or off and why (no clip, viewpoint, condition, volume curve, frequency).
#[unsafe(no_mangle)]
pub extern "C" fn oo_sound_report(bus: *mut OoBus) {
    with_bus(bus, (), |b| {
        let Some(g) = global() else { return };
        let Some(ss) = b.sounds.as_ref() else {
            say(1, "sound report: the bus has no sound set");
            return;
        };
        let v = &b.v;
        let lines = ss.report(&g.audio, &|n| v.var(n));
        let heard = lines.iter().filter(|l| l.contains(" heard")).count();
        let no_clip = lines.iter().filter(|l| l.contains("no clip")).count();
        say(0, &format!("sound report: {} entries, {heard} heard, {no_clip} without clip, voices {}, listener {:?}",
            lines.len(), g.audio.voice_count(), g.audio.listener_position()));
        for l in lines.iter().filter(|l| l.contains(" heard")) {
            say(0, &format!("  {l}"));
        }
    })
}


/// Log the rigid body's wheels (ground, compression, load, walls met, step force) and the body.
#[unsafe(no_mangle)]
pub extern "C" fn oo_debug_wheels(bus: *mut OoBus) {
    with_bus(bus, (), |b| {
        let Some(rb) = b.v.rigid.as_ref() else {
            say(1, "debug: no rigid body");
            return;
        };
        say(0, &format!("debug body: pos {:?} vel {:?} omega {:?} holding {} speed {:.2} km/h",
            rb.position, rb.velocity, rb.omega, rb.holding, b.v.physics.velocity_kmh()));
        for (i, w) in rb.wheels.iter().enumerate() {
            say(0, &format!("  wheel {i}: ground_z {:.3} seen {} on_ground {} touch {:?} compression {:.3} load {:.0} walls {} step_force {:.0} spin {:.2} locked {} slipping {} lat_defl {:.3}",
                w.ground_z, w.ground_seen, w.on_ground, w.touch, w.compression, w.load, w.walls.len(), w.step_force, w.spin, w.locked, w.slipping, w.lat_defl));
            for f in w.walls.iter().take(3) {
                say(0, &format!("    wall {f:?}"));
            }
        }
    })
}

/// The lit lamps (`[light_enh]`, `[light_enh_2]`) of a vehicle and the sections it tows, in the world:
/// x, y, z, direction x, y, z (0 for an omni light), r, g, b (0..1), size x level. A lamp is lit while its
/// script variable is (headlights, brake, blinkers, reversing light: what openOMSI shows as the lamp's glow).
pub(crate) fn vehicle_lamps(v: &VehicleInstance, out: &mut Vec<[f32; 10]>, cap: usize) {
    let level = |n: &str| -> f32 {
        if n.trim().is_empty() {
            1.0
        } else {
            v.var(n).unwrap_or(0.0).clamp(0.0, 1.0)
        }
    };
    let norm = |c: [f32; 3]| if c.iter().any(|x| *x > 1.0) { [c[0] / 255.0, c[1] / 255.0, c[2] / 255.0] } else { c };
    let mut part = |pos: DVec3, ty: &VehicleType, xf: &dyn Fn(usize) -> glam::Mat4| {
        for (mi, me) in ty.meshes.iter().enumerate() {
            let Some(d) = ty.model.meshes.get(me.def_index) else { continue };
            if d.light_enh.is_empty() && d.light_enh_2.is_empty() {
                continue;
            }
            if !d.visible_by(|n| v.var(n)).unwrap_or(true) {
                continue;
            }
            let m = glam::Mat4::from_translation(pos.as_vec3()) * xf(mi);
            for l in &d.light_enh_2 {
                let lv = level(&l.variable);
                if lv < 0.05 || out.len() >= cap {
                    continue;
                }
                let p = m.transform_point3(Vec3::from(l.pos));
                let dir = if l.omni { Vec3::ZERO } else { m.transform_vector3(Vec3::from(l.dir)).normalize_or_zero() };
                let c = norm(l.color);
                out.push([p.x, p.y, p.z, dir.x, dir.y, dir.z, c[0], c[1], c[2], l.size.max(0.05) * lv]);
            }
            for l in &d.light_enh {
                let lv = level(&l.variable);
                if lv < 0.05 || out.len() >= cap {
                    continue;
                }
                let p = m.transform_point3(Vec3::from(l.pos));
                let c = norm(l.color);
                out.push([p.x, p.y, p.z, 0.0, 0.0, 0.0, c[0], c[1], c[2], l.size.max(0.05) * lv]);
            }
        }
    };
    part(v.position, &v.ty, &|i| v.mesh_local_transform(i));
    for tp in &v.trailers {
        part(tp.position, &tp.ty, &|i| tp.mesh_local_transform(i));
    }
}

/// `[smoothskin]` train mesh i as its `[setbone]` bones hold it now (openOMSI `skin_vertices`: the bones move
/// the vertices first, the mesh's own motion comes on top - Omsi.exe's order): 6 floats a vertex (position,
/// normal) in the mesh's own frame, in the file's vertex order. Returns the vertex count (0: not skinned, or
/// more than `cap`).
#[unsafe(no_mangle)]
pub extern "C" fn oo_bus_skinned(bus: *mut OoBus, i: i32, out: *mut f32, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let Some((part, k)) = train_mesh(b, i.max(0) as usize) else { return 0 };
        let n_vars = b.v.state.vars.len();
        let got = match part {
            None => b.v.skinned(k),
            Some(t) => b.v.trailers.get_mut(t).and_then(|tp| tp.skinned(k, n_vars)),
        };
        let Some((pos, nrm)) = got else { return 0 };
        if pos.len() > cap {
            return 0;
        }
        let dst = unsafe { std::slice::from_raw_parts_mut(out, pos.len() * 6) };
        for (v, p) in pos.iter().enumerate() {
            let n = nrm.get(v).copied().unwrap_or(Vec3::Z);
            dst[v * 6..v * 6 + 6].copy_from_slice(&[p.x, p.y, p.z, n.x, n.y, n.z]);
        }
        pos.len()
    })
}

/// The bus's lit `[light_enh]` / `[light_enh_2]` coronas (see `vehicle_lamps`), 10 floats each: glows to draw at
/// the lamps, never lights. Returns how many.
#[unsafe(no_mangle)]
pub extern "C" fn oo_bus_coronas(bus: *mut OoBus, out: *mut f32, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let mut v = Vec::new();
        vehicle_lamps(&b.v, &mut v, cap);
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr() as *const f32, out, v.len() * 10) };
        v.len()
    })
}

/// The bus's lit `[interiorlight]`s (front section, then each coupled section), 8 floats each: world x y z,
/// range (m), r g b (0..1), level (its variable, 0..1). They light only the meshes that name them in
/// `[illumination_interior]` (openOMSI 06_lights.wgsl: OMSI's Direct3D point light, 1 / (d² / range²)).
#[unsafe(no_mangle)]
pub extern "C" fn oo_bus_interior_lights(bus: *mut OoBus, out: *mut f32, cap: usize) -> usize {
    if out.is_null() || cap == 0 {
        return 0;
    }
    with_bus(bus, 0, |b| {
        let mut v: Vec<[f32; 8]> = Vec::new();
        // (lamp: position, range, colour, variable)
        let mut part = |lamps: Vec<([f32; 3], f32, [f32; 3], String)>, origin: DVec3, rot: glam::Mat4, val: &dyn Fn(&str) -> f32| {
            for (pos, range, color, var) in lamps {
                if v.len() >= cap {
                    return;
                }
                let lv = val(var.trim()).clamp(0.0, 1.0);
                if lv < 0.02 {
                    continue;
                }
                let p = origin.as_vec3() + rot.transform_point3(Vec3::from(pos));
                let c = color;
                let c = if c.iter().any(|x| *x > 1.0) { [c[0] / 255.0, c[1] / 255.0, c[2] / 255.0] } else { c };
                v.push([p.x, p.y, p.z, range.max(0.1), c[0], c[1], c[2], lv]);
            }
        };
        let val = |n: &str| -> f32 { b.v.var(n).unwrap_or(0.0) };
        let lamps = b.v.ty.model.interior_lights.iter().map(|l| (l.pos, l.range, l.color, l.variable.clone())).collect();
        part(lamps, b.v.position, b.v.body_rotation(), &val);
        for t in &b.v.trailers {
            let tval = |n: &str| -> f32 { t.run_var(n).unwrap_or_else(|| b.v.var(n).unwrap_or(0.0)) };
            let lamps = t.ty.model.interior_lights.iter().map(|l| (l.pos, l.range, l.color, l.variable.clone())).collect();
            part(lamps, t.position, t.body_rotation(), &tval);
        }
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr() as *const f32, out, v.len() * 8) };
        v.len()
    })
}

/// The bus's lit lamps (see `vehicle_lamps`), 10 floats each, at most `cap`. Returns how many.
#[unsafe(no_mangle)]
pub extern "C" fn oo_bus_lamps(bus: *mut OoBus, out: *mut f32, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    with_bus(bus, 0, |b| {
        // The player's bus lights the world with its headlights only: the `[spotlight]` its `Spot_Select`
        // picks (-1 off), as Omsi.exe's one player spot (openOMSI omsi-scene lights.rs). Its `[light_enh]` /
        // `[light_enh_2]` are coronas - glows that light nothing; fed as lamps, the thirty saloon ceiling
        // lamps of the AGG300 lit the street and the trees through its own roof.
        let mut v: Vec<[f32; 10]> = Vec::new();
        let sel = b.v.var("Spot_Select").unwrap_or(-1.0);
        if sel >= 0.0 && cap > 0 {
            if let Some(sp) = b.v.ty.model.spotlights.get(sel as usize) {
                let s = sp.values;
                let body = b.v.body_rotation();
                let p = b.v.position.as_vec3() + body.transform_point3(Vec3::new(s[0], s[1], s[2]));
                let d = body.transform_vector3(Vec3::new(s[3], s[4], s[5])).normalize_or_zero();
                let c = if s[6] > 1.0 || s[7] > 1.0 || s[8] > 1.0 { [s[6] / 255.0, s[7] / 255.0, s[8] / 255.0] } else { [s[6], s[7], s[8]] };
                // (a left and a right lamp at the bus's front corners, the spot's colour)
                let half = b.v.ty.def.bounding_box.map(|bb| bb[0] * 0.5 * 0.8).unwrap_or(1.0);
                let side = body.transform_vector3(Vec3::X).normalize_or_zero() * half;
                for q in [p - side, p + side] {
                    if v.len() < cap {
                        v.push([q.x, q.y, q.z, d.x, d.y, d.z, c[0], c[1], c[2], 0.6]);
                    }
                }
            }
        }
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr() as *const f32, out, v.len() * 10) };
        v.len()
    })
}
