void SoundRuntime::bind_vars(const std::vector<float>* public_floats, const omsi::script::VarRegistry* vars) {
    floats_ = public_floats;
    vars_ = vars;
}

void SoundRuntime::fire_trigger(const std::string& trigger_id, std::string_view via) {
    // OMSI matches (T.L.name) against sound.cfg [trigger] case-insensitively.
    const auto same = [](std::string_view x, std::string_view y) {
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(x[i])) != std::tolower(static_cast<unsigned char>(y[i])))
                return false;
        return true;
    };
    for (const auto& t : triggers_) {
        if (!same(t.trigger_id, trigger_id)) continue;

        const auto now = std::chrono::steady_clock::now();
        const auto min_iv =
            is_gear_oneshot_trigger(trigger_id) ? kOneshotGearInterval : kOneshotMinInterval;
        if (const auto it = oneshot_last_fire_.find(trigger_id); it != oneshot_last_fire_.end()) {
            if (now - it->second < min_iv) return;
        }
        oneshot_last_fire_[trigger_id] = now;

        const float gain = std::clamp(t.base_volume * eval_vol_curves(t.vol_curves, 0.f), 0.f, 1.f);
        if (gain < kAudibleEps) return;

        const std::string via_s = via.empty() ? "direct" : std::string(via);
        const int n = ++oneshot_dbg_count_[trigger_id];
        // Log first 3 plays, then every 30th — avoids matrix_frame spam drowning the log.
        if (n <= 3 || (n % 30) == 0) {
            omsi::log::info("OMSI Sound DBG oneshot via=" + via_s + " (T.L." + trigger_id + ") -> " +
                            omsi::path_text(t.wav_path.filename()) + " gain=" + std::to_string(gain) +
                            (n > 3 ? (" (x" + std::to_string(n) + ")") : ""));
        }
        enqueue(PlayCommand{.kind = PlayCommand::Kind::one_shot, .path = t.wav_path, .volume = gain});
        return;
    }
    if (g_missing_trigger_warned.insert(trigger_id).second) {
        const std::string via_s = via.empty() ? "direct" : std::string(via);
        omsi::log::warn("OMSI Sound: trigger not found '(T.L." + trigger_id + ")' via=" + via_s);
    }
}

void SoundRuntime::update(float dt_sec) {
    dbg_summary_age_ += dt_sec;
    int active_count = 0;
    std::string active_summary;

    for (auto& loop : loops_) {
        if (loop.was_active) loop.voice_age += dt_sec;
        // Gain from curves even when we will mute; condition only gates the voice lifetime.
        const float gain = loop_gain(loop);
        const float pitch = loop_pitch(loop);
        const bool want = loop_want_device(loop, dt_sec, gain);
        const float target_gain = want ? gain : 0.f;
        const float target_pitch = want ? pitch : 1.f;

        if (want && !loop.was_active) {
            loop.voice_age = 0.f;
            // Re-sample after age reset so [volcurve] -1 starts at t=0.
            const float start_gain = loop_gain(loop);
            const float start_pitch = loop_pitch(loop);
            loop.smooth_gain = start_gain;
            loop.smooth_pitch = start_pitch;
            if (loop.no_loop) {
                omsi::log::info("OMSI Sound DBG EDGE+ id=" + std::to_string(loop.loop_id) + " " +
                                describe_loop_reason(loop, start_gain, start_pitch) + " [noloop]");
                enqueue(PlayCommand{.kind = PlayCommand::Kind::one_shot,
                                    .path = loop.wav_path,
                                    .volume = start_gain,
                                    .pitch = start_pitch});
            } else {
                omsi::log::info("OMSI Sound DBG LOOP+ id=" + std::to_string(loop.loop_id) + " " +
                                describe_loop_reason(loop, start_gain, start_pitch));
                enqueue(PlayCommand{.kind = PlayCommand::Kind::loop_start,
                                    .path = loop.wav_path,
                                    .volume = start_gain,
                                    .pitch = start_pitch,
                                    .loop_id = loop.loop_id});
                loop.last_gain = start_gain;
                loop.last_pitch = start_pitch;
            }
        } else if (!want && loop.was_active) {
            if (!loop.no_loop) {
                omsi::log::info("OMSI Sound DBG LOOP- id=" + std::to_string(loop.loop_id) + " wav=" +
                                omsi::path_text(loop.wav_path.filename()) + " kind=" + loop.kind +
                                " via=update (silent/gated)");
                enqueue(PlayCommand{.kind = PlayCommand::Kind::loop_stop, .loop_id = loop.loop_id});
                loop.last_gain = -1.f;
                loop.last_pitch = -1.f;
                loop.smooth_gain = 0.f;
                loop.smooth_pitch = 1.f;
                loop.voice_age = 0.f;
            }
        } else if (want && !loop.no_loop) {
            loop.smooth_gain = approach(loop.smooth_gain, target_gain, kGainSlewPerSec, dt_sec);
            loop.smooth_pitch = approach(loop.smooth_pitch, target_pitch, kPitchSlewPerSec, dt_sec);
            const float out_gain = loop.smooth_gain;
            const float out_pitch = loop.smooth_pitch;
            const bool gain_changed = std::fabs(out_gain - loop.last_gain) > kGainUpdateEps;
            const bool pitch_changed = std::fabs(out_pitch - loop.last_pitch) > kPitchUpdateEps;
            if (gain_changed || pitch_changed) {
                enqueue(PlayCommand{.kind = PlayCommand::Kind::loop_params,
                                    .volume = out_gain,
                                    .pitch = out_pitch,
                                    .loop_id = loop.loop_id});
                loop.last_gain = out_gain;
                loop.last_pitch = out_pitch;
            }
        }

        const float report_gain = want ? loop.smooth_gain : 0.f;
        if (want && !loop.no_loop && report_gain >= kAudibleEps) {
            ++active_count;
            if (active_summary.size() < 220) {
                if (!active_summary.empty()) active_summary += " | ";
                active_summary += omsi::path_text(loop.wav_path.filename()) + " g=" +
                                  std::to_string(report_gain).substr(0, 4) + " p=" +
                                  std::to_string(loop.smooth_pitch).substr(0, 4);
            }
        }
        loop.was_active = want;
    }

    if (dbg_summary_age_ >= 5.f) {
        dbg_summary_age_ = 0.f;
        if (active_count > 0)
            omsi::log::info("OMSI Sound DBG active=" + std::to_string(active_count) + " [" + active_summary +
                            "]");
        if (mixer_) {
            omsi::log::info("OMSI Sound mixer ok=" + std::to_string(mixer_->ok) + " frames=" +
                            std::to_string(mixer_->frames_rendered.load()) + " voices=" +
                            std::to_string(mixer_->voice_count.load()) + " peak5s=" +
                            std::to_string(mixer_->peak.exchange(0.f)));
        } else {
            omsi::log::warn("OMSI Sound mixer missing");
        }
    }
}

float SoundRuntime::var_value(const std::string& name) const {
    if (!vars_ || !floats_) return 0.f;
    if (const auto idx = vars_->float_index(name)) return floats_->at(static_cast<std::size_t>(*idx));
    return 0.f;
}

float SoundRuntime::eval_vol_curves(const std::vector<VolCurve>& curves, float voice_age) const {
    if (curves.empty()) return 1.f;
    float gain = 1.f;
    for (const auto& vc : curves) {
        if (vc.points.empty()) {
            gain = 0.f;
            break;
        }
        const float x = vc.is_age ? voice_age : var_value(vc.var_name);
        gain *= vc.is_age ? sample_age_curve(vc.points, x) : sample_curve(vc.points, x);
        if (gain < kAudibleEps) return 0.f;
    }
    return gain;
}

float SoundRuntime::eval_vol_curves(const ConditionalLoop& c) const {
    return eval_vol_curves(c.vol_curves, c.voice_age);
}

float SoundRuntime::loop_gain(const ConditionalLoop& c) const {
    return std::clamp(c.base_volume * eval_vol_curves(c), 0.f, 1.f);
}

float SoundRuntime::loop_pitch(const ConditionalLoop& c) const {
    if (c.pitch_var.empty() || c.pitch_ref <= 1e-6f) return 1.f;
    const float v = var_value(c.pitch_var);
    // Negative RPM (broken engine state) must not invert/screech playback.
    if (v <= 0.f) return 0.25f;
    return std::clamp(v / c.pitch_ref, 0.125f, 4.f);
}

bool SoundRuntime::eval_condition(const ConditionalLoop& c) const {
    if (!c.has_condition || c.conditions.empty()) return true;
    constexpr float kEps = 1e-4f;
    for (const auto& cl : c.conditions) {
        const float v = var_value(cl.var_name);
        const float ref = cl.compare_value;
        bool ok = false;
        switch (cl.compare_op) {
        case 0: // <>
            ok = std::fabs(v - ref) >= kEps;
            break;
        case 1: // =
            ok = std::fabs(v - ref) < kEps;
            break;
        case 2: // <
            ok = v < ref;
            break;
        case 3: // >
            ok = v > ref;
            break;
        case 4: // <=
            ok = v <= ref + kEps;
            break;
        case 5: // >=
            ok = v >= ref - kEps;
            break;
        default:
            ok = std::fabs(v - ref) < kEps;
            break;
        }
        if (!ok) return false; // AND
    }
    return true;
}

bool SoundRuntime::loop_want_device(ConditionalLoop& c, float dt_sec, float gain) const {
    // Debounce conditionSingle: OMSI does not open/close the voice on 1-frame glitches.
    const bool raw = eval_condition(c);
    if (raw) {
        c.cond_false_age = 0.f;
        c.cond_true_age += dt_sec;
    } else {
        c.cond_true_age = 0.f;
        c.cond_false_age += dt_sec;
    }

    bool cond_ok = false;
    if (c.was_active) {
        // Stay on until the condition has been false for a hold period.
        cond_ok = raw || c.cond_false_age < kCondOffHoldSec;
    } else {
        cond_ok = raw && c.cond_true_age >= kCondOnHoldSec;
    }
    // No conditionSingle → always "on" from the condition side (volcurves mute).
    if (!c.has_condition) cond_ok = true;

    if (!cond_ok) {
        c.silent_age = 0.f;
        return false;
    }

    // OMSI: volume curves go to 0 without stopping the sample. We keep waveOut open and
    // mute; only release after a quiet hold so we do not thrash LOOP+/LOOP- on Acelerador.
    if (c.was_active) {
        if (gain < kAudibleStopEps) {
            // Age fade may stay at 0 for >1s (e.g. Lenta starter delay); do not free yet.
            float age_end = 0.f;
            bool has_age = false;
            for (const auto& vc : c.vol_curves) {
                if (!vc.is_age || vc.points.empty()) continue;
                has_age = true;
                age_end = std::max(age_end, vc.points.back().first);
            }
            if (has_age && c.voice_age <= age_end + 0.05f) {
                c.silent_age = 0.f;
                return true;
            }
            c.silent_age += dt_sec;
            return c.silent_age < kMuteHoldSec;
        }
        c.silent_age = 0.f;
        return true;
    }

    c.silent_age = 0.f;
    if (gain >= kAudibleEps) return true;
    // [volcurve] -1 often starts at 0; open muted only if the named curves would be
    // audible once the fade completes (avoids Lenta/idle LOOP+/− thrash when RPM
    // curves already force gain to 0).
    bool has_age = false;
    float named = 1.f;
    for (const auto& vc : c.vol_curves) {
        if (vc.is_age) {
            has_age = true;
            continue;
        }
        if (vc.points.empty()) {
            named = 0.f;
            break;
        }
        named *= sample_curve(vc.points, var_value(vc.var_name));
        if (named < kAudibleEps) {
            named = 0.f;
            break;
        }
    }
    return has_age && (c.base_volume * named) >= kAudibleEps;
}

std::string SoundRuntime::describe_loop_reason(const ConditionalLoop& c, float gain, float pitch) const {
    static constexpr const char* kOps[] = {"<>", "=", "<", ">", "<=", ">="};
    std::string s = "wav=" + omsi::path_text(c.wav_path.filename());
    s += " kind=" + (c.kind.empty() ? "?" : c.kind);
    s += " via=update";
    s += " gain=" + std::to_string(gain);
    s += " pitch=" + std::to_string(pitch);
    if (c.has_condition && !c.conditions.empty()) {
        for (std::size_t i = 0; i < c.conditions.size(); ++i) {
            const auto& cl = c.conditions[i];
            const char* op = (cl.compare_op >= 0 && cl.compare_op <= 5) ? kOps[cl.compare_op] : "?";
            if (i) s += " && ";
            else s += " cond=";
            s += cl.var_name + "(" + std::to_string(var_value(cl.var_name)) + ")" + op +
                 std::to_string(cl.compare_value);
        }
    } else {
        s += " cond=(none)";
    }
    if (!c.vol_curves.empty()) {
        s += " curves=";
        for (std::size_t i = 0; i < c.vol_curves.size(); ++i) {
            const auto& vc = c.vol_curves[i];
            const float x = vc.is_age ? c.voice_age : var_value(vc.var_name);
            const float y = vc.points.empty() ? 0.f
                                 : (vc.is_age ? sample_age_curve(vc.points, x) : sample_curve(vc.points, x));
            if (i) s += ",";
            const std::string label = vc.is_age ? "age" : vc.var_name;
            s += label + "(" + std::to_string(x) + ")->" + std::to_string(y);
        }
    }
    if (!c.pitch_var.empty()) {
        s += " pitch_var=" + c.pitch_var + "(" + std::to_string(var_value(c.pitch_var)) + ")/" +
             std::to_string(c.pitch_ref);
    }
    if (!c.linked_trigger.empty()) s += " linked_T.L.=" + c.linked_trigger;
    if (c.no_loop) s += " noloop";
    return s;
}
