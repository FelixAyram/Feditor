//! Headless check of the C ABI: spawn a bus on flat ground, auto-start it, hold the brake and
//! select D, then full throttle. usage: oo_drive_test <omsi_root> <bus> [seconds]
use std::ffi::{CString, c_char, c_void};

use omsi_vehicle_ffi::*;

extern "C" fn log_line(level: i32, msg: *const c_char) {
    let s = unsafe { std::ffi::CStr::from_ptr(msg) }.to_string_lossy();
    eprintln!("[{}] {s}", ["info", "warn", "error"][level.clamp(0, 2) as usize]);
}

extern "C" fn flat(_: *mut c_void, _: f64, _: f64, z: *mut f64) -> i32 {
    unsafe { *z = 0.0 };
    1
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let root = CString::new(a[1].clone()).unwrap();
    let bus_path = CString::new(a[2].clone()).unwrap();
    let secs: f32 = a.get(3).and_then(|s| s.parse().ok()).unwrap_or(40.0);
    assert_eq!(oo_init(root.as_ptr(), Some(log_line)), 1);
    if std::env::var_os("PROBE_ONLY").is_some() {
        let d = CString::new(std::env::var("PROBE_DIR").unwrap_or_default()).unwrap();
        oo_audio_probe(secs, d.as_ptr());
        std::thread::sleep(std::time::Duration::from_secs_f32(secs + 1.5));
        return;
    }
    let bus = oo_spawn(bus_path.as_ptr(), 0.0, 0.0, 0.0, 0.0);
    assert!(!bus.is_null());
    oo_set_ground(bus, Some(flat), std::ptr::null_mut());
    oo_set_obstacles(bus, std::ptr::null(), 0);
    let key = |name: &str, down: bool| {
        let c = CString::new(name).unwrap();
        oo_key(bus, c.as_ptr(), down as i32);
    };
    let dt = 1.0 / 30.0;
    let frames = (secs / dt) as usize;
    let mut st = OoStatus::default();
    if let Some(dir) = std::env::var_os("PROBE_DIR") {
        let d = CString::new(dir.to_string_lossy().to_string()).unwrap();
        oo_audio_probe(secs.min(30.0), d.as_ptr());
    }
    oo_autostart(bus);
    for i in 0..frames {
        let t = i as f32 * dt;
        // after the auto-start (engine running, D in, parking brake off): full throttle for 12 s,
        // then steer left for 4 s, then brake
        if (t - 30.0).abs() < dt / 2.0 {
            key("throttle", true);
        }
        if (t - 42.0).abs() < dt / 2.0 {
            key("steering_left", true);
        }
        if (t - 46.0).abs() < dt / 2.0 {
            key("steering_left", false);
            key("throttle", false);
            key("brake", true);
        }
        oo_tick(bus, dt, 0.0, -15.0, 3.0, 0.0, 1.0, 0.0);
        if i % 300 == 150 {
            oo_sound_report(bus);
        }
        if i % 60 == 0 || i + 1 == frames {
            oo_status(bus, &mut st);
            let mut p = [0f64; 6];
            oo_pose(bus, p.as_mut_ptr());
            println!(
                "t={t:5.1} rpm={:6.0} running={} power={} autostart={} kmh={:5.1} voices={} audio={} thr={:.2} brk={:.2} pos=({:.1},{:.1},{:.2}) hdg={:.1} pitch={:.2} bank={:.2}",
                st.engine_rpm, st.engine_running, st.power_on, st.autostart_running, st.speed_kmh, st.voices, st.audio_ok, st.throttle, st.brake, p[0], p[1], p[2], p[3], p[4], p[5]
            );
        }
        if std::env::var_os("REALTIME").is_some() {
            std::thread::sleep(std::time::Duration::from_secs_f32(dt));
        }
    }
    oo_despawn(bus);
}
