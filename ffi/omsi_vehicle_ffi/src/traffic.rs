//! openOMSI's AI road traffic (omsi-sim `TrafficSim`, AGPL-3.0) over a C ABI for the map editor.
//!
//! The editor hands over the lanes of the map it has loaded (its own spline / object paths, in its
//! own world frame, with their `[rule]`s) and the crossings' light programs (read here from their
//! `.sco`); the traffic is openOMSI's: the random car types of the map's `ailists.cfg`
//! (`unsched_vehgroups.txt` groups), each car a full `VehicleInstance` (scripts, animations, paint
//! schemes, coupled trailers) driven by openOMSI's planner - car following, lane changes, junction
//! priority, traffic lights. The population around the camera is a simplified form of openOMSI's
//! app-side `Traffic::populate`: cars appear out of sight within the spawn radius and go when far
//! away or when their way ends.

use std::ffi::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use glam::DVec3;
use omsi_sim::ai_traffic::TrafficSim;
use omsi_sim::ai_traffic::control::Removal;
use omsi_sim::ai_traffic::setup::RandomTypes;
use omsi_sim::traffic::{LaneBuilder, LaneKey, LaneKind, TrafficLightController};
use omsi_sim::{VehicleInstance, VehicleType};

use crate::{cstr, global, guard, say};

pub struct OoTraffic {
    sim: TrafficSim,
    root: PathBuf,
    ground: Option<Arc<dyn Fn(f64, f64) -> Option<f64> + Send + Sync>>,
    rng: u64,
    initial: bool,
    pop_t: f32,
    /// object id -> light program index (its first appearance numbers it)
    light_of_object: std::collections::HashMap<i64, usize>,
    /// the editor's bus as the traffic sees it (front section box), see oo_traffic_set_bus
    player: Option<omsi_sim::ai_traffic::model::PlayerBox>,
    /// the map's timetable buses: the vehicles of its ailists groups with a .hof or a depot (read on first use)
    map_dir: PathBuf,
    service_files: Option<Vec<String>>,
    service_types: std::collections::HashMap<String, Arc<VehicleType>>,
    /// the editor camera's view (direction x y z, vertical fov in degrees, aspect), see oo_traffic_set_view
    view: Option<([f64; 3], f32, f64)>,
    /// cars giving way: (seconds waited, the car it gave way to last, how many have gone by since); see `patience`
    yield_watch: std::collections::HashMap<u64, (f32, u64, u8, f32)>,
    /// cars whose driving was tuned for the editor (queue gap, reaction): see `tune_new_cars`
    tuned: std::collections::HashSet<u64>,
    /// `[busstop]` objects' platform side (their 6th string: 0 right, 1 the other, 2 both), see oo_traffic_stop_side
    stop_sides: std::collections::HashMap<i64, f32>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OoLaneDesc {
    pub kind: i32,
    pub width: f32,
    pub speed_limit_kmh: f32,
    pub density: f32,
    pub no_cars: i32,
    pub rule_bus: i32,
    pub rule_trucks: i32,
    pub no_overtaking: i32,
    pub turn: i32,
    pub priority: f32,
    pub source: i32,
    pub tile_x: i32,
    pub tile_y: i32,
    pub id: i64,
    pub path: i32,
    pub reversed: i32,
    pub light_object: i64,
    pub light_index: i32,
    pub n_points: i32,
    /// `[rule] trafficdensity` per AI group (`unsched_vehgroups.txt` index): `n_groups` (group, value)
    /// pairs from pair `group_first` of the groups array, in rule order (the last of a group counts)
    pub n_groups: i32,
    pub group_first: i32,
    /// a spline lane's lateral offset from the spline's middle (m, the .sli path's `lateral`): lanes of one
    /// spline side by side become each other's left / right neighbours (lane changes, a way round a queue)
    pub offset: f32,
    /// the object path's `[crossingproblem]`: its meeting places with the other paths are the whole lanes
    pub crossing_problem: i32,
}

fn next_rng(r: &mut u64) -> u64 {
    *r ^= *r << 13;
    *r ^= *r >> 7;
    *r ^= *r << 17;
    *r
}

/// The map's random car types (openOMSI app `random_types`, simplified: the `[aigroup_2]` groups of
/// `ailists.cfg` that `unsched_vehgroups.txt` names - or its default group - no depots, no trains).
fn random_types(root: &Path, map_dir: &Path) -> RandomTypes {
    let mut types = Vec::new();
    let mut groups = Vec::new();
    let mut group_uvg = Vec::new();
    let mut uvg_defaults = Vec::new();
    let unscheduled: Option<Vec<(String, i32)>> = omsi_cfg::CfgFile::read(&map_dir.join("unsched_vehgroups.txt"))
        .ok()
        .map(|f| omsi_map::ailists::parse_unsched_vehgroups(&f).into_iter().map(|(n, c)| (n.trim().to_ascii_lowercase(), c)).collect());
    if let Some(names) = &unscheduled {
        uvg_defaults = names.iter().map(|n| n.1).collect();
    }
    let dens: Vec<omsi_map::ailists::UnschedGroup> = omsi_cfg::CfgFile::read(&map_dir.join("unsched_trafficdens.txt"))
        .ok()
        .map(|f| omsi_map::ailists::parse_unsched_trafficdens(&f))
        .unwrap_or_default();
    let group_curves = !dens.is_empty();
    let lists = match omsi_map::ailists::AiLists::load(&map_dir.join("ailists.cfg")) {
        Ok(l) => l,
        Err(e) => {
            say(1, &format!("traffic: no ailists.cfg in {}: {e}", map_dir.display()));
            return RandomTypes { types, groups, group_curves, group_uvg, uvg_defaults };
        }
    };
    for (gi, g) in lists.groups.iter().enumerate() {
        if g.is_depot || g.hof.is_some() || g.vehicles.iter().any(|v| v.file.to_ascii_lowercase().ends_with(".zug")) {
            continue;
        }
        let lname = g.name.trim().to_ascii_lowercase();
        let uvg = match &unscheduled {
            Some(names) => match names.iter().position(|n| n.0 == lname) {
                Some(u) => Some(u),
                None => continue,
            },
            None if gi == lists.default_group => None,
            None => continue,
        };
        let gidx = groups.len();
        group_uvg.push(uvg);
        groups.push(dens.iter().find(|d| d.name.trim().eq_ignore_ascii_case(g.name.trim())).cloned().unwrap_or(
            omsi_map::ailists::UnschedGroup { name: g.name.clone(), factor: if group_curves { 0.0 } else { 1.0 }, densities: Vec::new() },
        ));
        for v in &g.vehicles {
            let lower = v.file.to_ascii_lowercase();
            if lower.ends_with(".zug") || lower.contains("trains\\") || lower.contains("trains/") {
                continue;
            }
            let path = omsi_cfg::resolve_path(root, &v.file);
            match VehicleType::load_ai(root, &path) {
                Ok(t) => {
                    if t.def.is_rail() || matches!(t.def.kind, omsi_vehicle::vehicle::VehicleKind::Other(3)) {
                        continue;
                    }
                    types.push((Arc::new(t), v.weight.max(0.0), LaneKind::Street, gidx));
                }
                Err(e) => say(1, &format!("traffic: AI vehicle {}: {e}", v.file)),
            }
        }
    }
    say(0, &format!("traffic: {} AI vehicle types in {} groups", types.len(), groups.len()));
    RandomTypes { types, groups, group_curves, group_uvg, uvg_defaults }
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_create(map_dir: *const c_char, target: i32, spawn_radius: f32, left_hand: i32) -> *mut OoTraffic {
    guard(std::ptr::null_mut(), || {
        let Some(g) = global() else {
            say(2, "oo_traffic_create before oo_init");
            return std::ptr::null_mut();
        };
        let Some(dir) = (unsafe { cstr(map_dir) }) else { return std::ptr::null_mut() };
        let dir = PathBuf::from(dir);
        let random = random_types(&g.root, &dir);
        // (right before left between unsigned crossing paths: see Network::right_before_left)
        let net = omsi_sim::traffic::Network { left_hand: left_hand != 0, right_before_left: true, ..Default::default() };
        let mut sim = TrafficSim::assemble(
            &g.root,
            net,
            random,
            Vec::new(),
            Default::default(),
            (Vec::new(), Vec::new()),
            Vec::new(),
            (1.0, 0),
            target.max(0) as usize,
        );
        sim.spawn_radius = spawn_radius.clamp(80.0, 2000.0) as f64;
        sim.no_timetable_buses = true;
        Box::into_raw(Box::new(OoTraffic {
            sim,
            root: g.root.clone(),
            map_dir: dir.clone(),
            service_files: None,
            service_types: std::collections::HashMap::new(),
            ground: None,
            rng: 0x9E37_79B9_7F4A_7C15,
            initial: true,
            pop_t: 0.0,
            light_of_object: Default::default(),
            player: None,
            view: None,
            yield_watch: Default::default(),
            tuned: Default::default(),
            stop_sides: Default::default(),
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_destroy(t: *mut OoTraffic) {
    if !t.is_null() {
        guard((), || drop(unsafe { Box::from_raw(t) }));
    }
}

fn with_t<T>(t: *mut OoTraffic, fallback: T, f: impl FnOnce(&mut OoTraffic) -> T) -> T {
    if t.is_null() {
        return fallback;
    }
    guard(fallback, || f(unsafe { &mut *t }))
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_set_target(t: *mut OoTraffic, target: i32, spawn_radius: f32) {
    with_t(t, (), |t| {
        t.sim.target = target.max(0) as usize;
        t.sim.spawn_radius = spawn_radius.clamp(80.0, 2000.0) as f64;
    })
}

/// The ground under the AI cars (as oo_set_ground for the bus).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_set_ground(t: *mut OoTraffic, f: Option<crate::OoGroundFn>, user: *mut c_void) {
    with_t(t, (), |t| {
        t.ground = f.map(|f| {
            let cb = crate::GroundCb { f, user };
            Arc::new(move |x: f64, y: f64| {
                let cb = cb;
                let mut z = 0.0f64;
                if (cb.f)(cb.user, x, y, &mut z) != 0 { Some(z) } else { None }
            }) as Arc<dyn Fn(f64, f64) -> Option<f64> + Send + Sync>
        });
    })
}

/// The light program of crossing object `object_id` (its `.sco`'s `[trafficlight]`s), numbered the
/// first time; returns its index, -1 when the object has none.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_light_program(t: *mut OoTraffic, object_id: i64, sco_path: *const c_char) -> i32 {
    let Some(p) = (unsafe { cstr(sco_path) }) else { return -1 };
    with_t(t, -1, |t| {
        if let Some(&i) = t.light_of_object.get(&object_id) {
            return i as i32;
        }
        let sco = match omsi_scenery::SceneryObject::load(Path::new(p)) {
            Ok(s) => s,
            Err(_) => return -1,
        };
        if sco.traffic_lights.is_empty() {
            return -1;
        }
        let program = sco.traffic_lights.iter().map(|l| (l.phases.iter().map(|p| (p.state, p.duration)).collect(), l.approach_dist)).collect();
        let c = TrafficLightController::from_program(program, sco.traffic_lights_group, &sco.traffic_light_stop, &sco.traffic_light_jump);
        let idx = t.sim.lights.len();
        t.sim.light_prev.push(vec![-100; c.lights.len()]);
        t.sim.lights.push(c);
        t.sim.controller_of_object.insert(object_id, idx);
        t.light_of_object.insert(object_id, idx);
        idx as i32
    })
}

/// Lanes into the network (linked where they meet, the existing ones keep their indices). `pts`:
/// x, y, z of every lane's points one after the other (`n_points` each).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_add_lanes(t: *mut OoTraffic, lanes: *const OoLaneDesc, n: usize, pts: *const f32, groups: *const f32) -> i32 {
    if lanes.is_null() || pts.is_null() || n == 0 {
        return 0;
    }
    with_t(t, 0, |t| {
        let descs = unsafe { std::slice::from_raw_parts(lanes, n) };
        let total: usize = descs.iter().map(|d| d.n_points.max(0) as usize).sum();
        let all = unsafe { std::slice::from_raw_parts(pts, total * 3) };
        let mut at = 0usize;
        let mut new = Vec::with_capacity(n);
        for d in descs {
            let np = d.n_points.max(0) as usize;
            let p: Vec<DVec3> = (0..np).map(|k| DVec3::new(all[(at + k) * 3] as f64, all[(at + k) * 3 + 1] as f64, all[(at + k) * 3 + 2] as f64)).collect();
            at += np;
            if p.len() < 2 {
                continue;
            }
            let kind = LaneKind::from_code(d.kind);
            let mut l = LaneBuilder::polyline(p, kind, if d.width > 0.1 { d.width } else { 3.0 });
            if d.speed_limit_kmh > 0.0 {
                l.speed_limit_kmh = d.speed_limit_kmh;
            }
            l.density = d.density.max(0.0);
            if d.n_groups > 0 && !groups.is_null() && d.group_first >= 0 {
                let g = unsafe { std::slice::from_raw_parts(groups.add(d.group_first as usize * 2), d.n_groups as usize * 2) };
                l.group_density = g.chunks(2).map(|c| (c[0].max(0.0) as u16, c[1])).collect();
            }
            l.no_cars = d.no_cars != 0;
            l.rule_bus = d.rule_bus != 0;
            l.rule_trucks = d.rule_trucks != 0;
            l.no_overtaking = d.no_overtaking != 0;
            l.turn = d.turn;
            if d.priority >= 0.0 {
                l.priority = d.priority;
            }
            l.source = d.source.max(0) as u32;
            l.offset = d.offset;
            l.crossing_problem = d.crossing_problem != 0;
            l.key = Some(LaneKey { tile: (d.tile_x, d.tile_y), id: d.id, path: d.path.max(0) as u16 });
            l.reversed = d.reversed != 0;
            if d.light_object >= 0 && d.light_index >= 0 {
                if let Some(&c) = t.light_of_object.get(&d.light_object) {
                    l.traffic_light = Some((c, d.light_index as usize));
                }
            }
            new.push(l);
        }
        let added = t.sim.net.extend(new, omsi_sim::traffic::LINK_TOLERANCE);
        let count = added.len();
        t.sim.set_spawn_densities(added);
        t.sim.lanes_generation += 1;
        count as i32
    })
}

/// Keep the population around (cx, cy, cz): cars out of range go; new ones appear out of sight.
fn populate(t: &mut OoTraffic, center: DVec3, forward: DVec3) {
    let far = t.sim.spawn_radius * 1.6;
    let f2 = glam::DVec2::new(forward.x, forward.y).normalize_or_zero();
    let mut i = 0;
    while i < t.sim.cars.len() {
        let c = &t.sim.cars[i];
        let p = c.vehicle.position;
        let to = glam::DVec2::new(p.x - center.x, p.y - center.y);
        let d = to.length();
        let lane_next_empty = t.sim.net.lanes.get(c.state.lane).map(|l| l.next.is_empty()).unwrap_or(true);
        let at_end = c.gone && c.stopped > 0.5 && c.state.route.is_empty() && lane_next_empty;
        let out_of_view = d > 60.0 && to.normalize_or_zero().dot(f2) < 0.2;
        // (a car whose way ends - the end of the lanes handed over so far - waits where it is for the next
        //  lanes; it goes only once nobody sees it, never in plain view)
        // (a timetable bus stays while it serves its trip: it goes once the trip is done and nobody sees it)
        let service = c.bus.is_some();
        let remove = if service {
            (c.bus.as_ref().is_some_and(|b| b.trip_done()) && out_of_view) || d > far * 3.0
        } else {
            d > far || ((at_end || c.gone) && out_of_view) || (c.stopped > 240.0 && out_of_view)
        };
        if remove {
            let _ = t.sim.take_off(i, Removal::Finished);
        } else {
            i += 1;
        }
    }
    // the map's traffic at this hour (unsched_trafficdens.txt curves per group, or global.cfg's curve): the
    // cars wanted are the editor's number times that, never under 15 % of it
    let dens_now = t.sim.street_density();
    let target = if dens_now.is_finite() && dens_now > 0.0 {
        ((t.sim.target as f32) * dens_now.clamp(0.15, 2.0)).round() as usize
    } else {
        t.sim.target
    };
    if t.sim.cars.len() >= target || t.sim.types.is_empty() || t.sim.net.lanes.is_empty() {
        return;
    }
    let lanes = t.sim.net.lanes_starting_near(center, t.sim.spawn_radius);
    if lanes.is_empty() {
        return;
    }
    let mut spawned = 0;
    let budget = if t.initial { 40 } else { 3 };
    for _ in 0..(budget * 12) {
        if t.sim.cars.len() >= target || spawned >= budget {
            break;
        }
        let r = next_rng(&mut t.rng);
        let li = lanes[(r % lanes.len() as u64) as usize];
        let (ok, len) = {
            let l = &t.sim.net.lanes[li];
            (l.kind == LaneKind::Street && l.density > 0.0 && l.length() >= 8.0 && l.traffic_light.is_none() && l.source != 2, l.length())
        };
        if !ok {
            continue;
        }
        // (a lane brings traffic as much as its density says: one at 0.2 gets a car a fifth as often as one at 1)
        let dens = t.sim.net.lanes[li].density;
        if dens < 1.0 && (next_rng(&mut t.rng) % 1000) as f32 / 1000.0 > dens {
            continue;
        }
        let s = ((next_rng(&mut t.rng) % 10_000) as f32 / 10_000.0) * (len - 4.0).max(0.0) + 2.0;
        let (pos, heading) = t.sim.net.lanes[li].at(s);
        let to = glam::DVec2::new(pos.x - center.x, pos.y - center.y);
        let d = to.length();
        if d > t.sim.spawn_radius || (!t.initial && d < 70.0) {
            continue;
        }
        // (out of sight: not ahead of the camera within its view, except far off)
        if !t.initial && d < 260.0 && to.normalize_or_zero().dot(f2) > 0.35 {
            continue;
        }
        // the type as openOMSI picks it (density.rs pick_type): only the vehicles the lane is open to ([rule]
        // trucks / buses / no_cars, Lane::allows - Omsi.exe 0x71d714), of the groups the lane carries
        // ([rule] trafficdensity per unsched_vehgroups.txt group) as much as each group makes at this hour
        // (unsched_trafficdens.txt). By weight alone, trucks came out on every street and in yards.
        let Some(ty) = t.sim.pick_type(LaneKind::Street, Some(li)) else { continue };
        if !t.sim.spawn_clear(&ty, pos, heading as f64) || !t.sim.spawn_gap(pos) {
            continue;
        }
        let seed = next_rng(&mut t.rng);
        let scheme = if ty.paint_schemes.is_empty() { None } else { t.sim.ai_scheme(&ty, seed >> 8) };
        let mut host = omsi_sim::VehicleHost::new(omsi_sim::SimClock::default());
        host.paint_scheme = Some(scheme);
        let mut vehicle = VehicleInstance::new(ty.clone(), host);
        vehicle.ground = t.ground.clone();
        vehicle.apply_paint_vars(scheme);
        crate::attach_coupled_parts(&t.root, &mut vehicle);
        t.sim.place_car(vehicle, LaneKind::Street, li, s, ty, seed, scheme, None, None, None);
        spawned += 1;
    }
    t.initial = false;
}

/// One step of the traffic (dt seconds) seen from the camera; `player` (may be null): the bus as
/// centre x, y, z, heading (deg), half length, half width, speed (m/s).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_tick(t: *mut OoTraffic, dt: f32, cx: f64, cy: f64, cz: f64, fx: f64, fy: f64, player: *const f64, day_hour: f32) {
    with_t(t, (), |t| {
        let dt = dt.clamp(0.0, 0.1);
        let center = DVec3::new(cx, cy, cz);
        let fwd = DVec3::new(fx, fy, 0.0).normalize_or(DVec3::Y);
        t.sim.camera = Some(center);
        // openOMSI's viewer: a car out of the picture (and beyond the mirrors' and shadows' reach) runs its
        // script a few times a second instead of every step and leaves its animations (set_visibility)
        if let Some((d, fov, aspect)) = t.view {
            let f = DVec3::new(d[0], d[1], d[2]);
            t.sim.viewer = Some(omsi_sim::ai_traffic::viewer::Viewer::from_camera(center, if f.length() > 1e-6 { f } else { fwd }, fov * 1.25, 3000.0, aspect, 3000.0));
        }
        t.sim.day_time = (day_hour.clamp(0.0, 24.0) as f64) * 3600.0;
        t.pop_t += dt;
        if t.initial || t.pop_t >= 0.25 {
            t.pop_t = 0.0;
            populate(t, center, fwd);
        }
        let pb = if player.is_null() {
            t.player
        } else {
            let p = unsafe { std::slice::from_raw_parts(player, 7) };
            Some((DVec3::new(p[0], p[1], p[2]), p[3], p[4] as f32, p[5] as f32, p[6] as f32))
        };
        tune_new_cars(t);
        t.sim.tick(dt, pb);
        patience(t, dt);
    })
}

/// A queue closes up to 1.5 - 2.5 m and moves off half a second after the car ahead (openOMSI's drivers kept
/// 1.6 - 3 m plus 0.75 m and reacted in 0.8 s: queues stood long and loose). Once per car.
fn tune_new_cars(t: &mut OoTraffic) {
    if t.tuned.len() > 20_000 {
        let alive: std::collections::HashSet<u64> = t.sim.cars.iter().map(|c| c.id).collect();
        t.tuned.retain(|id| alive.contains(id));
    }
    for c in &mut t.sim.cars {
        if !t.tuned.insert(c.id) {
            continue;
        }
        let r = ((c.id.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 40) % 1000) as f32 / 1000.0;
        c.state.min_gap = c.state.min_gap.min(1.5 + 1.0 * r);
        c.state.reaction = c.state.reaction.min(0.4 + 0.25 * r);
    }
}

/// A driver giving way takes his turn either after 10 s or after 2 to 4 cars have gone by (one rule or the
/// other, by driver): openOMSI's deadlock pass (`yield_pass`) - the right of way stops holding him for a
/// while; bodies in the way, people, lights and a full exit still do, so nobody is driven into anybody.
/// 30 s of giving way in all (the passes that did not get him going count on) is the limit for every driver
/// not held by a light: then the cars he waits for that stand themselves give up their claims on the junction
/// (a circle of drivers each waiting for the other's claim never broke: one stood 63 s), and he is let go
/// again every few seconds until he moves.
fn patience(t: &mut OoTraffic, dt: f32) {
    use omsi_sim::ai_traffic::deadlock::Hold;
    const WAIT: f32 = 10.0;
    const CAP: f32 = 30.0;
    const PASS: f32 = 6.0;
    let now = t.sim.time;
    let mut seen = std::collections::HashSet::new();
    let mut release: Vec<u64> = Vec::new();
    for c in &mut t.sim.cars {
        // (never at a light: the lights decide there, and a pass does not take a car through a red anyway)
        let standing = c.vehicle.physics.speed.abs() < 1.0;
        let giving_way = (c.yielding || c.why.0 == "yield" || c.why.0 == "merge") && !c.light_hold && standing;
        if !giving_way {
            continue;
        }
        seen.insert(c.id);
        // (seconds giving way, the car it gave way to last, how many went by since, when it was last let go)
        let e = t.yield_watch.entry(c.id).or_insert((0.0, 0, 0, f32::MIN));
        e.0 += dt;
        let other = c.yield_to.or(c.waits_on).unwrap_or(0);
        if other != 0 && other != e.1 {
            if e.1 != 0 {
                e.2 = e.2.saturating_add(1); // (the one it waited for went by: the next one holds it now)
            }
            e.1 = other;
        }
        if c.yield_pass > now {
            continue; // (a pass is running)
        }
        // one rule per driver, by its id: half of them take their turn after 10 s of giving way, the other
        // half once 2 to 4 cars have gone by
        let h = c.id.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 33;
        let by_time = h % 2 == 0;
        let need = 2 + ((h >> 1) % 3) as u8;
        let turn = if by_time { e.0 > WAIT } else { e.2 >= need };
        let first = e.3 == f32::MIN;
        let stuck = e.0 > CAP && now - e.3 > PASS;
        if (turn && first) || stuck {
            c.yield_pass = now + PASS;
            e.3 = now;
            if e.0 > CAP {
                for (o, why) in &c.holds {
                    if !matches!(why, Hold::Body) {
                        release.push(*o);
                    }
                }
                if let Some(o) = c.yield_to.or(c.waits_on) {
                    release.push(o);
                }
            }
        }
    }
    if !release.is_empty() {
        for c in &mut t.sim.cars {
            // (only of one that stands and is not at a light: a car rolling through keeps its way)
            if release.contains(&c.id) && c.vehicle.physics.speed.abs() < 0.5 && !c.light_hold {
                c.reserved.clear();
                c.exit_claim = None;
            }
        }
    }
    t.yield_watch.retain(|id, _| seen.contains(id));
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_lane_count(t: *mut OoTraffic) -> i32 {
    with_t(t, 0, |t| t.sim.net.lanes.len() as i32)
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_count(t: *mut OoTraffic) -> i32 {
    with_t(t, 0, |t| t.sim.cars.len() as i32)
}

/// Car i: its id (stable while it lives) and how many meshes its train has (front + coupled parts).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car(t: *mut OoTraffic, i: i32, id: *mut u64, mesh_count: *mut i32) -> i32 {
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.get(i.max(0) as usize) else { return 0 };
        if !id.is_null() {
            unsafe { *id = c.id };
        }
        if !mesh_count.is_null() {
            let n = c.vehicle.ty.meshes.len() + c.vehicle.trailers.iter().map(|p| p.ty.meshes.len()).sum::<usize>();
            unsafe { *mesh_count = n as i32 };
        }
        1
    })
}

fn car_mesh(v: &VehicleInstance, k: usize) -> Option<(Option<usize>, usize)> {
    let n0 = v.ty.meshes.len();
    if k < n0 {
        return Some((None, k));
    }
    let mut r = k - n0;
    for (ti, p) in v.trailers.iter().enumerate() {
        if r < p.ty.meshes.len() {
            return Some((Some(ti), r));
        }
        r -= p.ty.meshes.len();
    }
    None
}

/// The .bus / .ovh file of mesh k's part of car i (UTF-8): the editor loads that part's model once.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_part_file(t: *mut OoTraffic, i: i32, k: i32, buf: *mut c_char, cap: usize) -> usize {
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.get(i.max(0) as usize) else { return 0 };
        let Some((part, _)) = car_mesh(&c.vehicle, k.max(0) as usize) else { return 0 };
        let ty = match part {
            None => &c.vehicle.ty,
            Some(p) => &c.vehicle.trailers[p].ty,
        };
        crate::write_cstr(&ty.def.path.to_string_lossy(), buf, cap)
    })
}

/// Mesh k's .o3d file of car i (UTF-8).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_mesh_file(t: *mut OoTraffic, i: i32, k: i32, buf: *mut c_char, cap: usize) -> usize {
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.get(i.max(0) as usize) else { return 0 };
        let Some((part, m)) = car_mesh(&c.vehicle, k.max(0) as usize) else { return 0 };
        let ty = match part {
            None => &c.vehicle.ty,
            Some(p) => &c.vehicle.trailers[p].ty,
        };
        match ty.meshes.get(m) {
            Some(me) => crate::write_cstr(&me.file.to_string_lossy(), buf, cap),
            None => 0,
        }
    })
}

/// Mesh k of car i in the world (DirectXMath row-vector matrix, row-major) and whether it shows.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_mesh_world(t: *mut OoTraffic, i: i32, k: i32, out16: *mut f32) -> i32 {
    if out16.is_null() {
        return 0;
    }
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.get(i.max(0) as usize) else { return 0 };
        let v = &c.vehicle;
        let Some((part, m)) = car_mesh(v, k.max(0) as usize) else { return 0 };
        let (mat, ty) = match part {
            None => (glam::Mat4::from_translation(v.position.as_vec3()) * v.mesh_local_transform(m), &v.ty),
            Some(p) => {
                let tp = &v.trailers[p];
                (glam::Mat4::from_translation(tp.position.as_vec3()) * tp.mesh_local_transform(m), &tp.ty)
            }
        };
        unsafe { std::ptr::copy_nonoverlapping(mat.to_cols_array().as_ptr(), out16, 16) };
        let vis = ty.meshes.get(m).and_then(|me| ty.model.meshes.get(me.def_index)).and_then(|d| d.visible_by(|n| v.var(n))).unwrap_or(true);
        if vis { 1 } else { 2 }
    })
}

/// A line of statistics: cars, lanes, light programs, and how many wait at a red light.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_stats(t: *mut OoTraffic, buf: *mut c_char, cap: usize) -> usize {
    with_t(t, 0, |t| {
        let s = format!(
            "cars={} lanes={} lights={} held_at_red={} types={}",
            t.sim.cars.len(),
            t.sim.net.lanes.len(),
            t.sim.lights.len(),
            t.sim.held_at_red,
            t.sim.types.len()
        );
        crate::write_cstr(&s, buf, cap)
    })
}

/// The editor's bus for the traffic: its front section's `[boundingbox]` as the player's box and every
/// coupled section's as an obstacle of its own (openOMSI app `own_outlines`: seeing only the front, the
/// traffic drove into the back of a turning articulated bus). Null: no bus. Call before each tick.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_set_bus(t: *mut OoTraffic, bus: *mut crate::OoBus) {
    with_t(t, (), |t| {
        t.sim.others.retain(|o| o.0 < 0xFFFF_0000);
        t.player = None;
        if bus.is_null() {
            return;
        }
        let b = unsafe { &*bus };
        let v = &b.v;
        let speed = v.physics.speed;
        let bb = v.ty.def.bounding_box.unwrap_or([2.5, 11.0, 3.0, 0.0, 0.0, 1.5]);
        t.player = Some(omsi_sim::ai_traffic::light_paths::box_outline(v.position, v.heading, bb, speed));
        for (k, tp) in v.trailers.iter().enumerate() {
            if let Some(bb) = tp.ty.def.bounding_box {
                t.sim.others.push((0xFFFF_0001 + k as u32, omsi_sim::ai_traffic::light_paths::box_outline(tp.position, tp.heading, bb, speed)));
            }
        }
    })
}

/// Script variable `name` of car i (its lights, brakes, blinkers: what its light materials follow).
/// Take the car of id `id` off the road (deleted in the editor). Returns 1 when it was on it.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_remove_car(t: *mut OoTraffic, id: u64) -> i32 {
    with_t(t, 0, |t| i32::from(t.sim.take_off_id(id, Removal::Finished).is_some()))
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_var(t: *mut OoTraffic, i: i32, name: *const c_char, fallback: f32) -> f32 {
    let Some(name) = (unsafe { cstr(name) }) else { return fallback };
    with_t(t, fallback, |t| t.sim.cars.get(i.max(0) as usize).and_then(|c| c.vehicle.var(name)).unwrap_or(fallback))
}

/// The text a `[texttexture]` field of car i shows now (UTF-8): its script's string variable (a timetable bus's
/// line and destination, its number plate) or a built-in string - openOMSI `text_texture_string`, as for the
/// player's bus (`oo_bus_texttex_string`). A coupled rear section reads the leading vehicle's strings, as in
/// openOMSI. Returns the bytes written.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_texttex_string(t: *mut OoTraffic, i: i32, field: *const c_char, buf: *mut c_char, cap: usize) -> usize {
    let Some(field) = (unsafe { cstr(field) }) else { return 0 };
    with_t(t, 0, |t| match t.sim.cars.get(i.max(0) as usize) {
        Some(c) => crate::write_cstr(&c.vehicle.text_texture_string(field.trim()), buf, cap),
        None => 0,
    })
}

/// Night (the street lights on): openOMSI's traffic then writes `AI_Light` (and `AI_Interiorlight`) 1 to every
/// car, which its model switches its head / tail lamps, night maps and `[light_enh]` coronas on. Never set
/// before, every AI car drove dark at night (only the brake lights' coronas showed).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_set_night(t: *mut OoTraffic, night: i32) {
    with_t(t, (), |t| t.sim.night = night != 0)
}

/// The lit lamps of the cars within `radius` of (cx, cy) (see `vehicle_lamps`), 10 floats each, at most
/// `cap`, the nearest cars first. Returns how many.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_lamps(t: *mut OoTraffic, cx: f64, cy: f64, radius: f64, out: *mut f32, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    with_t(t, 0, |t| {
        let mut order: Vec<(f64, usize)> = t
            .sim
            .cars
            .iter()
            .enumerate()
            .map(|(i, c)| ((c.vehicle.position.x - cx).powi(2) + (c.vehicle.position.y - cy).powi(2), i))
            .filter(|(d2, _)| *d2 <= radius * radius)
            .collect();
        order.sort_by(|a, b| a.0.total_cmp(&b.0));
        let mut v = Vec::new();
        for (_, i) in order {
            if v.len() >= cap {
                break;
            }
            crate::vehicle_lamps(&t.sim.cars[i].vehicle, &mut v, cap);
        }
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr() as *const f32, out, v.len() * 10) };
        v.len()
    })
}

/// Every car and towed section on the road: x, y, half length, half width (4 floats each, at most `cap`).
/// The editor's bus autopilot keeps its distance to them along its own route.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_bodies(t: *mut OoTraffic, out: *mut f32, cap: usize) -> usize {
    if out.is_null() {
        return 0;
    }
    with_t(t, 0, |t| {
        let mut v: Vec<[f32; 4]> = Vec::new();
        for c in &t.sim.cars {
            if v.len() >= cap {
                break;
            }
            let bb = c.vehicle.ty.def.bounding_box.unwrap_or([1.8, 4.5, 1.5, 0.0, 0.0, 0.7]);
            v.push([c.vehicle.position.x as f32, c.vehicle.position.y as f32, bb[1] * 0.5, bb[0] * 0.5]);
            for tp in &c.vehicle.trailers {
                if v.len() >= cap {
                    break;
                }
                let bb = tp.ty.def.bounding_box.unwrap_or([2.0, 6.0, 2.0, 0.0, 0.0, 1.0]);
                v.push([tp.position.x as f32, tp.position.y as f32, bb[1] * 0.5, bb[0] * 0.5]);
            }
        }
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr() as *const f32, out, v.len() * 4) };
        v.len()
    })
}

/// The paint of mesh k's part of car i: its `[CTC]` scheme's texture swaps as lines
/// "<default texture, lower case>\t<file>\n" (openOMSI `scheme_substitutions` for the car's scheme; a part
/// with no scheme chosen: the model's own look, `default_substitutions`). Returns the bytes written.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_paint(t: *mut OoTraffic, i: i32, k: i32, buf: *mut c_char, cap: usize) -> usize {
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.get(i.max(0) as usize) else { return 0 };
        let Some((part, _)) = car_mesh(&c.vehicle, k.max(0) as usize) else { return 0 };
        let ty = match part {
            None => &c.vehicle.ty,
            Some(p) => &c.vehicle.trailers[p].ty,
        };
        // A coupled part (an articulated bus's rear, a trailer) is painted in the front's scheme, as OMSI
        // repaints the whole consist: the scheme of the same name in the part's own [CTC] list, else the same
        // index (openOMSI `attach_trailers`) - before, only the front took the repaint and the rear kept the
        // model's own livery.
        let scheme = match (part, c.scheme) {
            (None, s) => s,
            (Some(_), Some(s)) => {
                let name = c.vehicle.ty.paint_schemes.get(s).map(|p| p.name.trim().to_ascii_lowercase());
                name.and_then(|n| ty.paint_schemes.iter().position(|p| p.name.trim().to_ascii_lowercase() == n))
                    .or_else(|| (s < ty.paint_schemes.len()).then_some(s))
            }
            (Some(_), None) => None,
        };
        let map = match scheme {
            Some(s) => ty.scheme_substitutions(s, omsi_texture::find_texture).0,
            None => ty.default_substitutions(&t.root),
        };
        let mut s = String::new();
        for (from, to) in map {
            s.push_str(&from);
            s.push('\t');
            s.push_str(&to);
            s.push('\n');
        }
        crate::write_cstr(&s, buf, cap)
    })
}

/// The phase light `light` of crossing `object_id` shows in the editor (its own program clock, the one the
/// lamps on screen follow, sped up with the editor's time scale): the traffic obeys exactly that. The
/// light's program becomes that one state until the next call. Returns 1 when the light exists.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_light_set(t: *mut OoTraffic, object_id: i64, light: i32, state: i32) -> i32 {
    with_t(t, 0, |t| {
        let Some(&ci) = t.light_of_object.get(&object_id) else { return 0 };
        let Some(c) = t.sim.lights.get_mut(ci) else { return 0 };
        let i = light.max(0) as usize;
        if i >= c.lights.len() {
            return 0;
        }
        if c.cycle != 3600.0 {
            // (driven from outside from now on: one long phase per light, no stop points of its own)
            c.cycle = 3600.0;
            c.stops.clear();
            c.held = false;
            c.time = 1.0;
        }
        if !(c.lights[i].len() == 1 && c.lights[i][0].0 == state) {
            c.lights[i] = vec![(state, 3600.0)];
        }
        1
    })
}


/// A timetable bus on its trip (openOMSI BusService): `entries` the trip's track as (path owner id, path index)
/// pairs, `stops` its stations as (x, y, object id, departure in seconds of the day) quadruples. The route is
/// the track's lanes as far as the network has them; a stop on it is served (doors, boarding, waiting for its
/// time). `bus_path` empty: one of the map's AI buses (`[ai_veh_type] 2`). Returns the car id, 0 when the
/// track has no lane here or no bus type was found.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_service_bus(
    t: *mut OoTraffic,
    bus_path: *const c_char,
    entries: *const i64,
    n_entries: usize,
    stops: *const f64,
    n_stops: usize,
    line: *const c_char,
    terminus: *const c_char,
) -> u64 {
    if entries.is_null() || n_entries == 0 {
        return 0;
    }
    let bus_path = unsafe { cstr(bus_path) }.unwrap_or("").to_string();
    let line = unsafe { cstr(line) }.unwrap_or("").to_string();
    let terminus = unsafe { cstr(terminus) }.unwrap_or("").to_string();
    let e = unsafe { std::slice::from_raw_parts(entries, n_entries * 2) };
    let st = if stops.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(stops, n_stops * 4) } };
    with_t(t, 0, |t| {
        let ty = service_type(t, &bus_path);
        let Some(ty) = ty else { return 0 };
        // the route: each entry's lane, the direction that goes on from the one before
        let net = &t.sim.net;
        let lanes_of = |id: i64, path: i64| -> Vec<usize> {
            net.lanes.iter().enumerate().filter(|(_, l)| l.key.is_some_and(|k| k.id == id && k.path as i64 == path)).map(|(i, _)| i).collect()
        };
        let mut route: Vec<usize> = Vec::new();
        for k in 0..n_entries {
            let cands = lanes_of(e[k * 2], e[k * 2 + 1]);
            if cands.is_empty() {
                if route.is_empty() {
                    continue; // (not handed over yet here: start where the network begins)
                }
                break;
            }
            let pick = if let Some(&prev) = route.last() {
                let end = net.lanes[prev].at(net.lanes[prev].length()).0;
                *cands.iter().min_by(|a, b| {
                    let da = net.lanes[**a].at(0.0).0.distance(end);
                    let db = net.lanes[**b].at(0.0).0.distance(end);
                    da.total_cmp(&db)
                }).unwrap()
            } else if k + 1 < n_entries {
                let next = lanes_of(e[(k + 1) * 2], e[(k + 1) * 2 + 1]);
                *cands.iter().min_by(|a, b| {
                    let end_a = net.lanes[**a].at(net.lanes[**a].length()).0;
                    let end_b = net.lanes[**b].at(net.lanes[**b].length()).0;
                    let da = next.iter().map(|n| net.lanes[*n].at(0.0).0.distance(end_a)).fold(f64::MAX, f64::min);
                    let db = next.iter().map(|n| net.lanes[*n].at(0.0).0.distance(end_b)).fold(f64::MAX, f64::min);
                    da.total_cmp(&db)
                }).unwrap()
            } else {
                cands[0]
            };
            if let Some(&prev) = route.last() {
                let end = net.lanes[prev].at(net.lanes[prev].length()).0;
                if net.lanes[pick].at(0.0).0.distance(end) > 6.0 {
                    break; // (a gap in what the network has: the route ends there for now)
                }
            }
            route.push(pick);
        }
        if route.is_empty() {
            return 0;
        }
        place_service(t, ty, route, st, line, terminus)
    })
}

fn place_service(t: &mut OoTraffic, ty: Arc<VehicleType>, route: Vec<usize>, st: &[f64], line: String, terminus: String) -> u64 {
    let net = &t.sim.net;
    // the stops on it, as Omsi.exe's AI bus comes to its [busstop] (TRoadVehicleInst_CalculatePAI 0x7dac5e..,
    // ported in openOMSI timetable_run::route): the stop object projected onto the route on its platform's side,
    // in the trip's order; the bus pulls over so that its kerb-side flank stands 0.3 m past the box's centre
    // (`bay_for`), and stops where its own [ai_brake_performance] hold puts its origin (`shift_stops`). (Before:
    // the nearest point of any lane of the route, on the lane's middle - buses stood in the road, short of or past
    // the stop, at the stop across the road on the way back.)
    let mut stop_list: Vec<(usize, f32, f32, f64, i64, f32)> = Vec::new();
    let mut from = 0usize;
    for q in st.chunks(4) {
        let id = q[2] as i64;
        let side = t.stop_sides.get(&id).copied().unwrap_or(0.0);
        let z = net.lanes[route[from.min(route.len() - 1)]].at(0.0).0.z;
        if let Some((ri, s, lat)) = net.project_stop_on_route_side(&route, DVec3::new(q[0], q[1], z), Some(25.0), from, side as u8) {
            stop_list.push((ri, s, lat, q[3], id, side));
            from = ri;
        }
    }
    omsi_sim::timetable_run::route::place_stops(net, &route, 0, &mut stop_list, &ty, false);
    stop_list.sort_by(|a, b| a.0.cmp(&b.0).then(a.1.total_cmp(&b.1)));
    let (pos, heading) = net.lanes[route[0]].at(1.0);
    if !t.sim.spawn_clear(&ty, pos, heading as f64) {
        // AI cars standing where the bus comes are taken off (they held a timetable bus off the road for good)
        let reach = ty.def.bounding_box.map(|b| b[1] as f64 * 0.5 + 4.0).unwrap_or(10.0);
        let under: Vec<u64> = t.sim.cars.iter().filter(|c| c.bus.is_none() && (c.vehicle.position - pos).truncate().length() < reach).map(|c| c.id).collect();
        for id in under {
            let _ = t.sim.take_off_id(id, Removal::Finished);
        }
        if !t.sim.spawn_clear(&ty, pos, heading as f64) {
            return 0;
        }
    }
    let seed = next_rng(&mut t.rng);
    let scheme = if ty.paint_schemes.is_empty() { None } else { t.sim.ai_scheme(&ty, seed >> 8) };
    let mut host = omsi_sim::VehicleHost::new(omsi_sim::SimClock::default());
    host.paint_scheme = Some(scheme);
    let mut vehicle = VehicleInstance::new(ty.clone(), host);
    vehicle.ground = t.ground.clone();
    vehicle.apply_paint_vars(scheme);
    crate::attach_coupled_parts(&t.root, &mut vehicle);
    let timetable = omsi_sim::ai_traffic::bus_service::AiTimetable {
        line,
        terminus,
        stops: stop_list.iter().map(|s| (s.4, String::new(), 0.0, 0.0)).collect(),
    };
    let setup = omsi_sim::ai_traffic::model::BusSetup {
        route: route.clone(),
        stops: stop_list.into_iter().map(omsi_sim::ai_traffic::bus_service::Stop::from_tuple).collect(),
        number: None,
        hof: None,
        timetable,
    };
    let r0 = route[0];
    // (no station of its trip on the way the network has yet: it drives on as traffic rather than ending its
    //  trip on the spot and standing there braked)
    let setup = if setup.stops.is_empty() { None } else { Some(setup) };
    t.sim.place_car(vehicle, LaneKind::Street, r0, 1.0, ty, seed, scheme, None, Some(0.0), setup)
}

fn service_type(t: &mut OoTraffic, bus_path: &str) -> Option<Arc<VehicleType>> {
    if !bus_path.is_empty() {
        let p = omsi_cfg::resolve_path(&t.root, bus_path);
        return VehicleType::load_ai(&t.root, &p).ok().map(Arc::new);
    }
    if t.service_files.is_none() {
        let mut files = Vec::new();
        if let Ok(lists) = omsi_map::ailists::AiLists::load(&t.map_dir.join("ailists.cfg")) {
            for g in &lists.groups {
                if !(g.is_depot || g.hof.is_some()) {
                    continue;
                }
                // (a depot's buses are its [aigroup_depot_typgroup_2] types)
                for f in g.vehicles.iter().map(|v| &v.file).chain(g.typgroups.iter().map(|tg| &tg.file)) {
                    let lower = f.to_ascii_lowercase();
                    if lower.ends_with(".bus") && !files.contains(f) {
                        files.push(f.clone());
                    }
                }
            }
        }
        say(0, &format!("traffic: {} timetable bus types in the map's ailists", files.len()));
        t.service_files = Some(files);
    }
    let files = t.service_files.clone().unwrap_or_default();
    for _ in 0..files.len().min(6) {
        let f = &files[(next_rng(&mut t.rng) % files.len() as u64) as usize];
        if let Some(ty) = t.service_types.get(f) {
            return Some(ty.clone());
        }
        let p = omsi_cfg::resolve_path(&t.root, f);
        if let Ok(ty) = VehicleType::load_ai(&t.root, &p) {
            let ty = Arc::new(ty);
            t.service_types.insert(f.clone(), ty.clone());
            return Some(ty);
        }
    }
    // (no timetable groups: one of the random traffic's buses)
    let buses: Vec<_> = t.sim.types.iter().filter(|x| x.0.def.ai_veh_type == 2).map(|x| x.0.clone()).collect();
    if buses.is_empty() {
        return None;
    }
    let i = (next_rng(&mut t.rng) % buses.len() as u64) as usize;
    Some(buses[i].clone())
}

/// A timetable bus on a trip without a track (OMSI type 2: stations only): it starts on the lane nearest
/// (sx, sy) and drives the shortest way over the network from station to station (`stops` as in
/// `oo_traffic_service_bus`). Returns the car id, 0 when nothing could be routed or placed.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_service_bus_stations(
    t: *mut OoTraffic,
    bus_path: *const c_char,
    sx: f64,
    sy: f64,
    stops: *const f64,
    n_stops: usize,
    line: *const c_char,
    terminus: *const c_char,
) -> u64 {
    if stops.is_null() || n_stops == 0 {
        return 0;
    }
    let bus_path = unsafe { cstr(bus_path) }.unwrap_or("").to_string();
    let line = unsafe { cstr(line) }.unwrap_or("").to_string();
    let terminus = unsafe { cstr(terminus) }.unwrap_or("").to_string();
    let st = unsafe { std::slice::from_raw_parts(stops, n_stops * 4) };
    with_t(t, 0, |t| {
        let Some(ty) = service_type(t, &bus_path) else { return 0 };
        let net = &t.sim.net;
        // the street lanes passing within 20 m of (x, y), nearest first (both directions: the stop's side decides)
        let near_all = |x: f64, y: f64| -> Vec<usize> {
            let p = DVec3::new(x, y, 0.0);
            let mut v: Vec<(f64, usize)> = net
                .lanes
                .iter()
                .enumerate()
                .filter(|(_, l)| l.kind == LaneKind::Street)
                .filter_map(|(i, l)| {
                    let mut pp = p;
                    pp.z = l.at(0.0).0.z;
                    l.nearest_point(pp).filter(|(_, d)| *d < 20.0).map(|(_, d)| (d, i))
                })
                .collect();
            v.sort_by(|a, b| a.0.total_cmp(&b.0));
            v.into_iter().take(6).map(|x| x.1).collect()
        };
        let starts = near_all(sx, sy);
        if starts.is_empty() {
            return 0;
        }
        // from the start lane that reaches the first station by the shortest way, on station by station
        let mut route: Vec<usize> = Vec::new();
        let mut cur: Option<usize> = None;
        for q in st.chunks(4) {
            let targets = near_all(q[0], q[1]);
            if targets.is_empty() {
                break;
            }
            let froms: Vec<usize> = match cur { Some(c) => vec![c], None => starts.clone() };
            let mut best: Option<Vec<usize>> = None;
            for &f in &froms {
                for &tg in &targets {
                    let path = if f == tg { Some(vec![f]) } else { net.shortest_path(f, tg) };
                    if let Some(path) = path {
                        if best.as_ref().is_none_or(|b| path.len() < b.len()) {
                            best = Some(path);
                        }
                    }
                }
            }
            let Some(path) = best else { break }; // (the way on is not handed over yet: the route ends here)
            for l in path {
                if route.last() != Some(&l) {
                    route.push(l);
                }
            }
            cur = route.last().copied();
        }
        // openOMSI's own router (timetable_run transit::route, as its timetable routes a trip): from where the
        // bus is through as many of its stations as the network has a way to now
        {
            let mut places = vec![DVec3::new(sx, sy, net.lanes[starts[0]].at(0.0).0.z)];
            for q in st.chunks(4) {
                places.push(DVec3::new(q[0], q[1], places[0].z));
            }
            let mut n = places.len();
            while n >= 2 {
                let names: Vec<String> = (0..n).map(|i| format!("s{i}")).collect();
                if let Ok(r) = omsi_sim::timetable_run::transit::route(net, omsi_cfg::transport::TransportMode::Bus, &names, &places[..n]) {
                    if r.lanes.len() > route.len() {
                        route = r.lanes;
                    }
                    break;
                }
                n -= 1;
            }
        }
        if route.is_empty() {
            route.push(starts[0]);
        }
        place_service(t, ty, route, st, line, terminus)
    })
}

/// One line about car `id` for the log: its timetable phase, speed, lane, route progress, standing time.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_debug(t: *mut OoTraffic, id: u64, buf: *mut c_char, cap: usize) -> usize {
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.iter().find(|c| c.id == id) else { return crate::write_cstr("gone", buf, cap) };
        let phase = c.bus.as_ref().map(|b| format!("{:?} {:.0}s", b.phase, b.phase_t)).unwrap_or_else(|| "-".into());
        let s = format!(
            "phase={} v={:.1}km/h lane={} route={}/{} stopped={:.1}s gone={} engine={:.0} throttle={:.2} brake={:.2}",
            phase,
            c.vehicle.physics.speed * 3.6,
            c.state.lane,
            c.state.route_index,
            c.state.route.len(),
            c.stopped,
            c.gone,
            c.vehicle.var("engine_on").unwrap_or(-1.0),
            c.vehicle.var("throttle").unwrap_or(-1.0),
            c.vehicle.var("brake").unwrap_or(-1.0)
        );
        crate::write_cstr(&s, buf, cap)
    })
}

/// What car `id` is doing now, for the editor's car window (cheap: read off its state, no search beyond the id).
#[repr(C)]
pub struct OoCarState {
    /// 0 driving, 1 accelerating, 2 braking, 3 waiting in a queue / for the car ahead, 4 red light, 5 giving way
    /// at a junction, 6 at a stop (timetable bus), 7 pulling out / parking, 8 end of its way, 9 waiting for room
    /// on its exit
    pub action: i32,
    pub speed_kmh: f32,
    /// seconds it has stood
    pub stopped_s: f32,
    /// distance to what holds it (m), -1 none
    pub why_dist: f32,
    /// the vehicle it waits for (id), 0 none
    pub waits_on: u64,
    /// the constraint nearest ahead ("lead", "light", "yield", "merge", ...), NUL terminated
    pub why: [u8; 16],
    /// a timetable bus's phase (Running, Boarding, Waiting, Closing, TripDone) and stops left, NUL terminated
    pub phase: [u8; 40],
}

fn put_str(dst: &mut [u8], s: &str) {
    let n = s.len().min(dst.len().saturating_sub(1));
    dst[..n].copy_from_slice(&s.as_bytes()[..n]);
    dst[n] = 0;
}

#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_state(t: *mut OoTraffic, id: u64, out: *mut OoCarState) -> i32 {
    if out.is_null() {
        return 0;
    }
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.iter().find(|c| c.id == id) else { return 0 };
        let o = unsafe { &mut *out };
        let v = c.vehicle.physics.speed;
        let throttle = c.vehicle.var("throttle").unwrap_or(0.0);
        let brake = c.vehicle.var("brake").unwrap_or(0.0);
        let at_stop = c.bus.as_ref().is_some_and(|b| {
            use omsi_sim::ai_traffic::bus_service::Phase;
            matches!(b.phase, Phase::Boarding | Phase::Waiting | Phase::Closing | Phase::TripDone)
        });
        let standing = v.abs() < 0.4;
        // (what holds it nearest ahead - `why` - first: a car giving way short of its light's line showed
        //  "semaforo en rojo" while a car at its right held it)
        let why = c.why.0;
        o.action = if at_stop {
            6
        } else if why == "light" {
            4
        } else if why == "yield" || why == "merge" || (c.yielding && why.is_empty()) {
            5
        } else if why == "lead" && standing {
            3
        } else if c.light_hold && why.is_empty() {
            4
        } else if c.exit_wait {
            9
        } else if c.pull_out > 0.0 || c.park.is_some() {
            7
        } else if standing && c.state.route.is_empty() && c.gone {
            8
        } else if standing && c.why.0 == "lead" {
            3
        } else if standing && (c.why.0 == "merge" || c.why.0 == "yield") {
            5
        } else if standing && (c.held || c.lead_car.is_some() || c.stopped > 0.5) {
            10 // (held by something else: `why` says what - "keep_back", "people", "service", ...)
        } else if c.state.braking || brake > 0.15 {
            2
        } else if throttle > 0.15 {
            1
        } else {
            0
        };
        o.speed_kmh = (v * 3.6) as f32;
        o.stopped_s = c.stopped;
        o.why_dist = if c.why.0.is_empty() { -1.0 } else { c.why.1 };
        o.waits_on = c.waits_on.or(c.yield_to).or(c.lead_car).unwrap_or(0);
        put_str(&mut o.why, c.why.0);
        let ph = c.bus.as_ref().map(|b| format!("{:?} {:.0}s, {} paradas", b.phase, b.phase_t, b.stops.len())).unwrap_or_default();
        put_str(&mut o.phase, &ph);
        1
    })
}

/// The body of car i in the world (model frame -> world, as `oo_body_matrix` for the player's bus).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_body(t: *mut OoTraffic, i: i32, out16: *mut f32) -> i32 {
    if out16.is_null() {
        return 0;
    }
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.get(i.max(0) as usize) else { return 0 };
        let m = glam::Mat4::from_translation(c.vehicle.position.as_vec3()) * c.vehicle.body_rotation();
        unsafe { std::ptr::copy_nonoverlapping(m.to_cols_array().as_ptr(), out16, 16) };
        1
    })
}

/// The editor camera's view for the traffic (direction to its target, vertical field of view in degrees,
/// width / height): cars out of it update their scripts and animations less often (openOMSI viewer).
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_set_view(t: *mut OoTraffic, dx: f64, dy: f64, dz: f64, fov_deg: f32, aspect: f32) {
    with_t(t, (), |t| {
        t.view = Some(([dx, dy, dz], fov_deg.clamp(10.0, 150.0), (aspect as f64).clamp(0.2, 5.0)));
    })
}

/// Why car `id` drives where it drives, for the editor's log when it is clicked: the lane it is on and the
/// lanes it has planned (as map paths: object/spline id, path, direction), its route if it has one, what holds
/// it and whom it waits for at its junction. One line; the editor logs it when it changes.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_car_explain(t: *mut OoTraffic, id: u64, buf: *mut c_char, cap: usize) -> usize {
    with_t(t, 0, |t| {
        let Some(c) = t.sim.cars.iter().find(|c| c.id == id) else { return crate::write_cstr("gone", buf, cap) };
        let net = &t.sim.net;
        let lane_txt = |l: usize| -> String {
            match net.lanes.get(l) {
                Some(ln) => {
                    let k = ln.key.map(|k| format!("{}{}/{}", if ln.source == 2 { "obj" } else { "spl" }, k.id, k.path)).unwrap_or_else(|| "?".into());
                    let turn = match ln.turn { 1 => "L", 2 => "R", _ => "" };
                    format!("{l}:{k}{}{turn}{}", if ln.reversed { "r" } else { "" }, if ln.next.is_empty() { "|END" } else { "" })
                }
                None => format!("{l}:-"),
            }
        };
        let st = &c.state;
        let mut s = format!("on {} at {:.0}/{:.0} m", lane_txt(st.lane), st.s, net.lanes.get(st.lane).map(|l| l.length()).unwrap_or(0.0));
        let nexts: Vec<String> = net.lanes.get(st.lane).map(|l| l.next.iter().map(|&n| lane_txt(n)).collect()).unwrap_or_default();
        s.push_str(&format!(" | exits [{}]", nexts.join(" ")));
        let mut plan: Vec<String> = Vec::new();
        if let Some(p) = st.planned_next {
            plan.push(lane_txt(p));
        }
        plan.extend(st.ahead.iter().take(6).map(|&l| lane_txt(l)));
        s.push_str(&format!(" | plan [{}]", plan.join(" > ")));
        if !st.route.is_empty() {
            let from = st.route_index.min(st.route.len());
            let r: Vec<String> = st.route[from..].iter().take(6).map(|&l| lane_txt(l)).collect();
            s.push_str(&format!(" | route {}/{} [{}]", st.route_index, st.route.len(), r.join(" > ")));
        }
        if st.turn_wish != 0 {
            s.push_str(&format!(" | turn_wish {}", st.turn_wish));
        }
        s.push_str(&format!(" | v {:.0} km/h why {} {:.0} m", c.vehicle.physics.speed * 3.6, c.why.0, c.why.1));
        if c.yielding { s.push_str(" yielding"); }
        if c.light_hold { s.push_str(" red"); }
        if c.exit_wait { s.push_str(" exit_full"); }
        if let Some(w) = c.waits_on { s.push_str(&format!(" waits_on {w}")); }
        if let Some(w) = c.yield_to { s.push_str(&format!(" yield_to {w}")); }
        if !c.holds.is_empty() {
            let h: Vec<String> = c.holds.iter().take(6).map(|(o, k)| format!("{o}:{k:?}")).collect();
            s.push_str(&format!(" holds [{}]", h.join(" ")));
        }
        if !c.reserved.is_empty() {
            s.push_str(&format!(" claims {}", c.reserved.len()));
        }
        if let Some(h) = &c.holding { s.push_str(&format!(" holding: {h}")); }
        if !c.junction_why.is_empty() { s.push_str(&format!(" junction: {}", c.junction_why)); }
        if c.stopped > 0.5 { s.push_str(&format!(" stood {:.0}s", c.stopped)); }
        crate::write_cstr(&s, buf, cap)
    })
}

/// The platform side of `[busstop]` object `id` (its 6th string, as OMSI reads it: 0 right, 1 the other, 2
/// both) for the timetable buses placed after this call.
#[unsafe(no_mangle)]
pub extern "C" fn oo_traffic_stop_side(t: *mut OoTraffic, id: i64, side: f32) {
    with_t(t, (), |t| {
        t.stop_sides.insert(id, if side.is_finite() { side.clamp(0.0, 2.0) } else { 0.0 });
    })
}
