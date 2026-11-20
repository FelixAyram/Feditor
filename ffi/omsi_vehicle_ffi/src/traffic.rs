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
        let net = omsi_sim::traffic::Network { left_hand: left_hand != 0, ..Default::default() };
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
            ground: None,
            rng: 0x9E37_79B9_7F4A_7C15,
            initial: true,
            pop_t: 0.0,
            light_of_object: Default::default(),
            player: None,
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
        if d > far || ((at_end || c.gone) && out_of_view) || (c.stopped > 240.0 && out_of_view) {
            let _ = t.sim.take_off(i, Removal::Finished);
        } else {
            i += 1;
        }
    }
    if t.sim.cars.len() >= t.sim.target || t.sim.types.is_empty() || t.sim.net.lanes.is_empty() {
        return;
    }
    let lanes = t.sim.net.lanes_starting_near(center, t.sim.spawn_radius);
    if lanes.is_empty() {
        return;
    }
    let total_w: f32 = t.sim.types.iter().map(|x| x.1.max(0.01)).sum();
    let mut spawned = 0;
    let budget = if t.initial { 40 } else { 3 };
    for _ in 0..(budget * 12) {
        if t.sim.cars.len() >= t.sim.target || spawned >= budget {
            break;
        }
        let r = next_rng(&mut t.rng);
        let li = lanes[(r % lanes.len() as u64) as usize];
        let (ok, len) = {
            let l = &t.sim.net.lanes[li];
            (l.kind == LaneKind::Street && !l.no_cars && l.density > 0.0 && l.length() >= 8.0 && l.traffic_light.is_none() && l.source != 2, l.length())
        };
        if !ok {
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
        let mut pick = (next_rng(&mut t.rng) % 1_000_000) as f32 / 1_000_000.0 * total_w;
        let mut ty = t.sim.types[0].0.clone();
        for x in &t.sim.types {
            pick -= x.1.max(0.01);
            if pick <= 0.0 {
                ty = x.0.clone();
                break;
            }
        }
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
        t.sim.tick(dt, pb);
    })
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
        let map = match (part, c.scheme) {
            (None, Some(s)) => ty.scheme_substitutions(s, omsi_texture::find_texture).0,
            _ => ty.default_substitutions(&t.root),
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
