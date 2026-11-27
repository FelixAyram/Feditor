GeoJobResult download_tile_aerial_satellite(const SatelliteProvider provider, const TileWgs84Bounds& bounds,
                                            const float tile_size_m, const int output_px,
                                            const std::filesystem::path& out_jpg, const std::string& api_key,
                                            const std::string& custom_url_template) {
    GeoJobResult r{};
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoScope {
        bool active = false;
        ~CoScope() {
            if (active) CoUninitialize();
        }
    } co_scope;
    co_scope.active = SUCCEEDED(co_hr);

    if (output_px < 64) {
        r.message = "Resolucion invalida";
        return r;
    }
    if (!wgs84_bounds_sane(bounds)) {
        r.message = "Bounds WGS84 invalidos — revisa ref lat/lon";
        return r;
    }
    if (provider == SatelliteProvider::MapTiler && api_key.empty()) {
        r.message = "MapTiler: clave gratis en maptiler.com";
        return r;
    }
    if (provider == SatelliteProvider::BingAerial && api_key.empty()) {
        r.message = "Bing: clave gratis en portal.azure.com (Bing Maps)";
        return r;
    }
    if (provider == SatelliteProvider::GeoportalPlOrto || provider == SatelliteProvider::GeoportalPlOrtoHiRes) {
        if (!bounds_in_poland(bounds)) {
            r.message = "Geoportal ORTO: solo Polonia (lat 49-55, lon 14-24.5)";
            return r;
        }
    }
    if (provider == SatelliteProvider::GcbaAerial2017 && !bounds_in_caba(bounds)) {
        r.message = "GCBA 2017: solo Ciudad de Buenos Aires (lat -34.5 a -34.8, lon -58.3 a -58.7)";
        return r;
    }
    if (provider == SatelliteProvider::IgnArgentinaVant && !bounds_in_argentina(bounds)) {
        r.message = "IGN VANT: solo Argentina (lat -55 a -21, lon -74 a -53)";
        return r;
    }
    if (provider == SatelliteProvider::CustomAerialUrl && custom_url_template.empty()) {
        r.message = "Custom URL: elige preset o escribe plantilla {z}/{x}/{y}";
        return r;
    }
    if (provider == SatelliteProvider::GoogleMapTiles) {
        r.message = "Usa download_tile_aerial_bmp para Google Map Tiles";
        return r;
    }
    if (provider == SatelliteProvider::GoogleStaticMaps) {
        r.message = "Usa download_tile_aerial_google_static";
        return r;
    }
    if (provider == SatelliteProvider::OmsiLinkAerial || provider == SatelliteProvider::GcmodsBing ||
        provider == SatelliteProvider::GcmodsGoogle) {
        r.message = "Usa download_tile_aerial_omsi para OMSI/GCMods link";
        return r;
    }

    const double lat_mid = (bounds.south + bounds.north) * 0.5;
    // Always size zoom from the WGS84 footprint (611.5·cos(lat) for world tiles), not a stale 300 m.
    float size_m = tile_metric_size_from_bounds(bounds);
    if (size_m < 1.f) size_m = tile_size_m > 1.f ? tile_size_m : kWorldEquatorTileMeters;
    // Google mt*: scale=2/4 serves 512/1024 px per XYZ tile (real detail, not upscale).
    const int google_mt_scale =
        (provider == SatelliteProvider::GoogleMtSatellite) ? ((output_px >= 4096) ? 4 : 2) : 1;
    const int tile_px = 256 * google_mt_scale;
    int z = pick_zoom_for_tile(size_m, lat_mid, output_px);
    // Guarantee crop has enough *image* pixels before resize (account for hi-DPI tiles).
    for (; z < 22; ++z) {
        const double span_px = std::abs(mercator_pixel_x(bounds.east, z) - mercator_pixel_x(bounds.west, z));
        if (span_px * static_cast<double>(google_mt_scale) >= static_cast<double>(output_px) * 0.98) break;
    }
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    constexpr std::int64_t kMaxAerialTiles = 1600; // 8K @ ~514 m → ~33×33 @ z21
    for (;;) {
        const TileXY t0 = latlon_to_tile(bounds.south, bounds.west, z);
        const TileXY t1 = latlon_to_tile(bounds.north, bounds.east, z);
        x0 = (std::min)(t0.x, t1.x);
        x1 = (std::max)(t0.x, t1.x);
        y0 = (std::min)(t0.y, t1.y);
        y1 = (std::max)(t0.y, t1.y);
        const int nx = x1 - x0 + 1;
        const int ny = y1 - y0 + 1;
        const std::int64_t bytes = static_cast<std::int64_t>(nx) * ny * tile_px * tile_px * 3;
        if (nx > 0 && ny > 0 && static_cast<std::int64_t>(nx) * ny <= kMaxAerialTiles &&
            bytes <= 512LL * 1024 * 1024)
            break;
        if (z <= 1) break;
        --z;
    }
    if (!aerial_tile_grid_ok(x0, x1, y0, y1, r, tile_px)) return r;

    const int tw = (x1 - x0 + 1) * tile_px;
    const int th = (y1 - y0 + 1) * tile_px;
    std::vector<std::uint8_t> mosaic(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 3, 30);

    struct TileJob {
        int tx = 0;
        int ty = 0;
        std::vector<std::uint8_t> rgb;
        UINT w = 0;
        UINT h = 0;
        bool ok = false;
    };
    std::vector<TileJob> jobs;
    jobs.reserve(static_cast<std::size_t>((x1 - x0 + 1) * (y1 - y0 + 1)));
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx) jobs.push_back(TileJob{tx, ty, {}, 0, 0, false});

    // Parallel HTTP + decode (WinHTTP sessions are per-call; ~16–32 workers cut 8K from minutes to seconds).
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n_workers =
        (std::min)(32u, (std::max)(8u, hw == 0 ? 16u : hw * 2u));
    std::atomic<std::size_t> next_job{0};
    std::atomic<int> got{0};
    std::vector<std::thread> pool;
    pool.reserve(n_workers);
    for (unsigned wi = 0; wi < n_workers; ++wi) {
        pool.emplace_back([&, provider, z, api_key, custom_url_template, google_mt_scale]() {
            const HRESULT whr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool co_own = SUCCEEDED(whr);
            for (;;) {
                const std::size_t i = next_job.fetch_add(1, std::memory_order_relaxed);
                if (i >= jobs.size()) break;
                auto& job = jobs[i];
                const std::wstring url = satellite_tile_url(provider, z, job.tx, job.ty, api_key,
                                                           custom_url_template, google_mt_scale);
                if (url.empty()) continue;
                const auto resp = http::http_get(url);
                if (resp.status != 200 || resp.body.empty()) continue;
                if (!decode_image_rgb(resp.body, job.rgb, job.w, job.h)) continue;
                job.ok = true;
                got.fetch_add(1, std::memory_order_relaxed);
            }
            if (co_own) CoUninitialize();
        });
    }
    for (auto& t : pool) t.join();

    for (const auto& job : jobs) {
        if (!job.ok) continue;
        blit_bgr(mosaic, tw, th, (job.tx - x0) * tile_px, (job.ty - y0) * tile_px, job.rgb,
                 static_cast<int>(job.w), static_cast<int>(job.h));
    }

    if (got.load() == 0) {
        r.message = "Sin tiles — revisa conexion o proveedor";
        return r;
    }

    std::vector<std::uint8_t> cropped;
    int crop_w = tw;
    int crop_h = th;
    if (!crop_mosaic_to_bounds(mosaic, tw, th, x0, y0, z, bounds, cropped, crop_w, crop_h, tile_px)) {
        cropped = mosaic;
        crop_w = tw;
        crop_h = th;
    }

    std::vector<std::uint8_t> out_rgb;
    resize_bilinear_bgr(cropped, crop_w, crop_h, out_rgb, output_px, output_px);
    std::filesystem::create_directories(out_jpg.parent_path());
    if (!write_jpeg_rgb(out_jpg, output_px, output_px, out_rgb)) {
        r.message = "Error escribiendo JPEG";
        return r;
    }

    r.ok = true;
    r.output_path = out_jpg;
    {
        std::ostringstream msg;
        msg << satellite_provider_label(provider) << " z=" << z;
        if (google_mt_scale > 1) msg << " scale=" << google_mt_scale << "(" << tile_px << "px)";
        msg << ' ' << output_px << "px tiles=" << got.load() << "/" << jobs.size()
            << " workers=" << n_workers << " size=" << std::fixed << std::setprecision(1) << size_m << "m"
            << " crop=" << crop_w << "x" << crop_h;
        r.message = msg.str();
    }
    if (provider == SatelliteProvider::GeoportalPlOrto) {
        write_aerial_sidecar(out_jpg, "Geoportal PL ORTO WMTS StandardResolution",
                             "orthophoto=latest aggregate (see geoportal.gov.pl skorowidze for flight dates)\r\n");
    } else if (provider == SatelliteProvider::GeoportalPlOrtoHiRes) {
        write_aerial_sidecar(out_jpg, "Geoportal PL ORTO WMTS HighResolution",
                             "orthophoto=hi-res aggregate (see geoportal.gov.pl skorowidze for flight dates)\r\n");
    } else if (provider == SatelliteProvider::GcbaAerial2017) {
        write_aerial_sidecar(out_jpg, "GCBA fotografias_aereas_2017_caba TMS",
                             "orthophoto=2017 CABA (GCBA USIG mapcache)\r\n");
    } else if (provider == SatelliteProvider::IgnArgentinaVant) {
        write_aerial_sidecar(out_jpg, "IGN Argentina mosaicos_vuelos_vant TMS",
                             "orthophoto=UAV mosaic (IGN Geoserver GWC)\r\n");
    } else if (provider == SatelliteProvider::GoogleMtSatellite) {
        std::ostringstream side;
        side << "experimental=1;scale=" << google_mt_scale << ";tile_px=" << tile_px
             << ";tos=violates Google Maps Platform ToS; local test only\r\n";
        write_aerial_sidecar(out_jpg, "Google mt*.google.com vt/lyrs=s&scale (unofficial)", side.str());
    }
    return r;
}

GeoJobResult download_tile_dtm_opentopodata(const TileWgs84Bounds& bounds, const int dim,
                                            std::vector<float>& out_heights) {
    GeoJobResult r{};
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }

    struct Sample {
        double lat = 0.;
        double lon = 0.;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(dim * dim));
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const double lat = bounds.south + (bounds.north - bounds.south) * fy;
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const double lon = bounds.west + (bounds.east - bounds.west) * fx;
            samples.push_back({lat, lon});
        }
    }

    out_heights.assign(samples.size(), 0.f);
    const char* datasets[] = {"aster30m", "srtm30m"};
    const char* used_dataset = datasets[0];
    bool fetched = false;

    for (const char* dataset : datasets) {
        std::size_t idx = 0;
        bool batch_ok = true;
        while (idx < samples.size()) {
            const std::size_t end = (std::min)(idx + static_cast<std::size_t>(100), samples.size());
            std::ostringstream q;
            q << "https://api.opentopodata.org/v1/" << dataset << "?locations=";
            q << std::fixed;
            for (std::size_t i = idx; i < end; ++i) {
                if (i > idx) q << '|';
                q << samples[i].lat << ',' << samples[i].lon;
            }
            const std::wstring url(q.str().begin(), q.str().end());
            const auto resp = http::http_get(url);
            if (resp.status != 200) {
                batch_ok = false;
                break;
            }
            const auto vals = json_elevation_values(body_to_string(resp.body));
            if (vals.size() != end - idx) {
                batch_ok = false;
                break;
            }
            for (std::size_t j = 0; j < vals.size(); ++j)
                out_heights[idx + j] = static_cast<float>(vals[j]);
            idx = end;
            if (idx < samples.size()) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        }
        if (!batch_ok) continue;

        int valid = 0;
        for (float h : out_heights)
            if (!std::isnan(h)) ++valid;
        if (valid >= static_cast<int>(samples.size()) * 9 / 10) {
            used_dataset = dataset;
            fetched = true;
            break;
        }
    }

    if (!fetched) {
        r.message = "OpenTopoData sin datos (aster30m/srtm30m) — revisa bounds";
        return r;
    }

    fill_nan_heights(out_heights, dim);

    float base = out_heights.front();
    for (float h : out_heights)
        if (!std::isnan(h)) {
            base = h;
            break;
        }
    for (auto& h : out_heights) h -= base;

    r.ok = true;
    r.message = std::string("OpenTopoData ") + used_dataset + " OK puntos=" + std::to_string(out_heights.size()) +
                " base=" + std::to_string(base) + "m";
    return r;
}

std::optional<TileWgs84Bounds> tile_wgs84_bounds(const GlobalMap& global, const MapTileLayout& layout,
                                                 const std::int32_t grid_x, const std::int32_t grid_y,
                                                 const double ref_lat, const double ref_lon) {
    TileWgs84Bounds b{};
    const auto kind = detect_map_coord_kind(global);
    if (kind == MapCoordKind::WorldCoordinates) {
        latlon_from_world_grid(grid_x, grid_y, b);
        return b;
    }
    if (kind == MapCoordKind::GlobalTileNames) {
        const auto* metric = layout.find_grid(grid_x, grid_y);
        if (!metric) return std::nullopt;
        const auto stem = metric->tile_path.stem().string();
        f4f23ma::f32 lat = 0.f;
        if (!parse_latitude_from_global_tile_name(stem, lat)) return std::nullopt;
        const double size_m = metric->size_meters;
        const double m_lat = meters_per_degree_lat();
        const double m_lon = meters_per_degree_lon(static_cast<double>(lat));
        b.south = static_cast<double>(lat);
        b.north = b.south + size_m / m_lat;
        const double west_m = static_cast<double>(grid_x) * size_m;
        b.west = ref_lon + west_m / m_lon;
        b.east = b.west + size_m / m_lon;
        return b;
    }
    float size_m = kStandardTileMeters;
    if (const auto* metric = layout.find_grid(grid_x, grid_y)) size_m = metric->size_meters;
    latlon_from_classic_grid(grid_x, grid_y, ref_lat, ref_lon, static_cast<double>(size_m), b);
    return b;
}

static EditorGeoSettings load_editor_geo_settings_raw(const std::filesystem::path& map_root, bool* found) {
    EditorGeoSettings s{};
    const auto path = map_root / ".omsi_f4_editor.cfg";
    const auto doc = cfg::load_cfg_file(path);
    if (found) *found = doc.has_value();
    if (!doc) return s;
    if (doc->contains("[geo]") && !cfg::cfg_section(*doc, "[geo]").empty()) {
        const auto& v = cfg::cfg_section(*doc, "[geo]")[0];
        if (!v.empty()) {
            try {
                s.satellite_provider = static_cast<SatelliteProvider>(std::stoi(v[0]));
            } catch (...) {
            }
        }
        if (v.size() > 1) {
            try {
                s.elevation_provider = static_cast<ElevationProvider>(std::stoi(v[1]));
            } catch (...) {
            }
        }
        if (v.size() > 2) s.maptiler_api_key = v[2];
        if (v.size() > 3) {
            try {
                s.ref_lat = std::stod(v[3]);
            } catch (...) {
            }
        }
        if (v.size() > 4) {
            try {
                s.ref_lon = std::stod(v[4]);
            } catch (...) {
            }
        }
        if (v.size() > 5) s.google_api_key = v[5];
        if (v.size() > 6) s.bing_api_key = v[6];
        if (v.size() > 7) s.custom_aerial_url = v[7];
        if (v.size() > 8) {
            try {
                s.custom_aerial_preset = std::stoi(v[8]);
            } catch (...) {
            }
        }
    }
    if (doc->contains("[google]") && !cfg::cfg_section(*doc, "[google]").empty()) {
        const auto& v = cfg::cfg_section(*doc, "[google]")[0];
        if (!v.empty()) s.google_api_key = v[0];
        if (v.size() > 1) {
            try {
                s.ref_lat = std::stod(v[1]);
            } catch (...) {
            }
        }
        if (v.size() > 2) {
            try {
                s.ref_lon = std::stod(v[2]);
            } catch (...) {
            }
        }
    }
    if (doc->contains("[Editor_LinkAerial]") && !cfg::cfg_section(*doc, "[Editor_LinkAerial]").empty()) {
        const auto& v = cfg::cfg_section(*doc, "[Editor_LinkAerial]")[0];
        if (!v.empty()) s.editor_link_aerial = v[0];
    }
    if (s.editor_link_aerial.empty()) s.editor_link_aerial = gcmods_bing_satellite_link();
    if (doc->contains("[aerial]") && !cfg::cfg_section(*doc, "[aerial]").empty()) {
        const auto& v = cfg::cfg_section(*doc, "[aerial]")[0];
        if (!v.empty()) {
            s.tile_aerial_visible = (v[0] == "1" || v[0] == "true" || v[0] == "yes");
        }
        if (v.size() > 1) {
            try {
                s.aerial_resolution_pos = std::stoi(v[1]);
                if (s.aerial_resolution_pos < 0) s.aerial_resolution_pos = 0;
                if (s.aerial_resolution_pos > 5) s.aerial_resolution_pos = 5;
            } catch (...) {
            }
        }
    }
    return s;
}

// Geo/aerial provider + API keys are the user's, not the map's: a global copy next to the exe fills whatever the
// map's own .omsi_f4_editor.cfg does not define, so keys and providers do not have to be re-entered for every map.
// ref_lat/ref_lon, aerial visibility and resolution stay per map (they describe that map).
static std::filesystem::path editor_geo_global_root() {
    return options::editor_ui_prefs_path().parent_path();
}

EditorGeoSettings load_editor_geo_settings(const std::filesystem::path& map_root) {
    bool map_found = false;
    EditorGeoSettings s = load_editor_geo_settings_raw(map_root, &map_found);
    const auto groot = editor_geo_global_root();
    if (groot.empty() || groot == map_root) return s;
    bool g_found = false;
    const EditorGeoSettings g = load_editor_geo_settings_raw(groot, &g_found);
    if (!g_found) return s;
    if (!map_found) {
        s.satellite_provider = g.satellite_provider;
        s.elevation_provider = g.elevation_provider;
        s.custom_aerial_preset = g.custom_aerial_preset;
    }
    if (s.maptiler_api_key.empty()) s.maptiler_api_key = g.maptiler_api_key;
    if (s.bing_api_key.empty()) s.bing_api_key = g.bing_api_key;
    if (s.google_api_key.empty()) s.google_api_key = g.google_api_key;
    if (s.custom_aerial_url.empty()) s.custom_aerial_url = g.custom_aerial_url;
    return s;
}

namespace {

bool editor_cfg_ends_with_ci(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (std::size_t i = 0; i < suf.size(); ++i) {
        const auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])));
        const auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(suf[i])));
        if (a != b) return false;
    }
    return true;
}

// Live map folder (strips *_actual / *_original). Prefs must survive workspace wipe of *_actual.
std::filesystem::path editor_cfg_live_map_root(const std::filesystem::path& map_root) {
    if (map_root.empty()) return map_root;
    const std::string name = map_root.filename().string();
    if (editor_cfg_ends_with_ci(name, "_actual") && name.size() > 7)
        return map_root.parent_path() / name.substr(0, name.size() - 7);
    if (editor_cfg_ends_with_ci(name, "_original") && name.size() > 9)
        return map_root.parent_path() / name.substr(0, name.size() - 9);
    return map_root;
}

std::vector<std::filesystem::path> editor_cfg_write_roots(const std::filesystem::path& map_root) {
    std::vector<std::filesystem::path> roots;
    if (!map_root.empty()) roots.push_back(map_root);
    const auto live = editor_cfg_live_map_root(map_root);
    if (!live.empty() && live != map_root) roots.push_back(live);
    return roots;
}

std::filesystem::path editor_cfg_resolve_path(const std::filesystem::path& map_root) {
    const auto primary = map_root / ".omsi_f4_editor.cfg";
    if (std::filesystem::exists(primary)) return primary;
    const auto live = editor_cfg_live_map_root(map_root) / ".omsi_f4_editor.cfg";
    if (std::filesystem::exists(live)) return live;
    return primary;
}

bool parse_cfg_bool(const std::string& x, bool fallback) {
    if (x == "1" || x == "true" || x == "yes") return true;
    if (x == "0" || x == "false" || x == "no") return false;
    return fallback;
}

void apply_vista_kv(EditorUiSettings& s, const std::string& key, const std::string& val) {
    if (key == "show_spline_ends") s.show_spline_ends = parse_cfg_bool(val, s.show_spline_ends);
    else if (key == "show_path_ends") s.show_path_ends = parse_cfg_bool(val, s.show_path_ends);
    else if (key == "show_path_rails") s.show_path_rails = parse_cfg_bool(val, s.show_path_rails);
    else if (key == "autofill_spline_ends") s.autofill_spline_ends = parse_cfg_bool(val, s.autofill_spline_ends);
    else if (key == "texmax256") {
        s.tex_max_256 = parse_cfg_bool(val, s.tex_max_256);
        s.tex_max_256_from_file = true;
    } else if (key == "min_screen_pct") {
        try {
            s.min_screen_size_pct = std::stof(val);
            // (0.6 was the default of 0.65 / 0.66, saved by every run: back to the new default 0)
            if (std::fabs(s.min_screen_size_pct - 0.6f) < 1e-4f) s.min_screen_size_pct = 0.f;
        } catch (...) {
        }
    } else if (key == "visual_radius_m") {
        try {
            s.editor_visual_radius_m = std::stof(val);
        } catch (...) {
        }
    } else if (key == "sim_hour") {
        try {
            s.sim_hour = std::stof(val);
            s.sim_hour_from_file = true;
        } catch (...) {
        }
    } else if (key == "path_style") {
        try {
            s.path_style = std::stoi(val) == 1 ? 1 : 0;
        } catch (...) {
        }
    } else if (key == "hide_onlyeditor_splines") s.hide_onlyeditor_splines = parse_cfg_bool(val, s.hide_onlyeditor_splines);
    else if (key == "language") {
        try {
            s.language = std::stoi(val) == 1 ? 1 : 0;
        } catch (...) {
        }
    } else if (key == "show_sco_origin") s.show_sco_origin = parse_cfg_bool(val, s.show_sco_origin);
    else if (key == "arrows_3d") s.arrows_3d = parse_cfg_bool(val, s.arrows_3d);
    else if (key == "hide_nodes") s.hide_nodes = parse_cfg_bool(val, s.hide_nodes);
    else if (key == "union_ball_pct") {
        try {
            s.union_ball_screen_pct = std::stof(val);
        } catch (...) {
        }
    } else if (key == "grad_circle_pct") {
        try {
            s.grad_circle_screen_pct = std::stof(val);
        } catch (...) {
        }
    } else if (key == "tile_radius") {
        try {
            s.editor_tile_radius = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "place_feedback") s.place_feedback = parse_cfg_bool(val, s.place_feedback);
    else if (key == "place_feedback_volume") {
        try {
            s.place_feedback_volume = std::stof(val);
        } catch (...) {
        }
    } else if (key == "sfx_pack") {
        if (!val.empty()) s.sfx_pack = val;
    } else if (key == "hints_ignore") s.hints_ignore = parse_cfg_bool(val, s.hints_ignore);
    else if (key == "spline_stick_chain") s.spline_stick_chain = parse_cfg_bool(val, s.spline_stick_chain);
    else if (key == "spline_fill_chain") s.spline_fill_chain = parse_cfg_bool(val, s.spline_fill_chain);
    else if (key == "terrain_brush_mode") {
        try {
            s.terrain_brush_mode = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "terrain_brush_speed") {
        try {
            s.terrain_brush_speed = std::stof(val);
        } catch (...) {
        }
    } else if (key == "sim_month") {
        try {
            s.sim_month = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "sim_day") {
        try {
            s.sim_day = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "sim_year") {
        try {
            s.sim_year = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "chrono_date") {
        try {
            s.chrono_date = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "chrono_map") {
        s.chrono_map = val;
    } else if (key == "time_running") {
        s.time_running = val == "1";
    } else if (key == "time_speed") {
        try {
            s.time_speed = std::stof(val);
        } catch (...) {
        }
    } else if (key == "timezone_h") {
        try {
            s.timezone_h = std::stof(val);
        } catch (...) {
        }
    } else if (key == "weather_id") {
        try {
            s.weather_id = std::stoi(val);
        } catch (...) {
        }
    } else if (key == "show_sun_shadows") s.show_sun_shadows = parse_cfg_bool(val, s.show_sun_shadows);
    else if (key == "shaft_plus") s.shaft_plus = parse_cfg_bool(val, s.shaft_plus);
    else if (key == "fog_enabled") s.fog_enabled = parse_cfg_bool(val, s.fog_enabled);
    else if (key == "parked_cars_on") s.parked_cars_on = parse_cfg_bool(val, s.parked_cars_on);
    else if (key == "show_onlyeditor_objects") s.show_onlyeditor_objects = parse_cfg_bool(val, s.show_onlyeditor_objects);
    else if (key == "shadow_distance_m") {
        try {
            s.shadow_distance_m = std::stof(val);
        } catch (...) {
        }
    }
    else if (key == "vsync") s.vsync = parse_cfg_bool(val, s.vsync);
    else if (key == "world_sound_enabled") s.world_sound_enabled = parse_cfg_bool(val, s.world_sound_enabled);
    else if (key == "world_sound_volume") {
        try {
            s.world_sound_volume = std::stof(val);
        } catch (...) {
        }
    }
    else if (key == "shadow_map_size") {
        try {
            s.shadow_map_size = static_cast<std::uint32_t>(std::stoul(val));
        } catch (...) {
        }
    }
}

bool write_editor_geo_cfg(const std::filesystem::path& map_root, const EditorGeoSettings& geo) {
    if (map_root.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(map_root, ec);
    const auto path = map_root / ".omsi_f4_editor.cfg";
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "[geo]\r\n";
    out << static_cast<int>(geo.satellite_provider) << "\r\n";
    out << static_cast<int>(geo.elevation_provider) << "\r\n";
    out << geo.maptiler_api_key << "\r\n";
    out << geo.ref_lat << "\r\n";
    out << geo.ref_lon << "\r\n";
    out << geo.google_api_key << "\r\n";
    out << geo.bing_api_key << "\r\n";
    out << geo.custom_aerial_url << "\r\n";
    out << geo.custom_aerial_preset << "\r\n";
    out << "\r\n[google]\r\n";
    out << geo.google_api_key << "\r\n";
    out << geo.ref_lat << "\r\n";
    out << geo.ref_lon << "\r\n";
    out << "\r\n[Editor_LinkAerial]\r\n";
    out << geo.editor_link_aerial << "\r\n";
    out << "\r\n[aerial]\r\n";
    out << (geo.tile_aerial_visible ? "1" : "0") << "\r\n";
    out << geo.aerial_resolution_pos << "\r\n";
    return static_cast<bool>(out);
}

bool write_editor_ui_cfg(const std::filesystem::path& path, const EditorUiSettings& ui) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Write to a temp file and swap it in: a crash/kill mid-write used to leave a truncated cfg, which then
    // loaded as "all defaults". Binary mode so the explicit \r\n is not expanded to \r\r\n.
    auto tmp = path;
    tmp += ".tmp";
    omsi::fsutil::break_link(tmp); // copy-on-write workspace: never write through a link
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    // [vista] = flechas/rails — global for every map; also mirrored in [ui] for compat.
    out << "[vista]\r\n";
    out << "show_spline_ends=" << (ui.show_spline_ends ? "1" : "0") << "\r\n";
    out << "show_path_ends=" << (ui.show_path_ends ? "1" : "0") << "\r\n";
    out << "show_path_rails=" << (ui.show_path_rails ? "1" : "0") << "\r\n";
    out << "autofill_spline_ends=" << (ui.autofill_spline_ends ? "1" : "0") << "\r\n";
    out << "texmax256=" << (ui.tex_max_256 ? "1" : "0") << "\r\n";
    out << "visual_radius_m=" << ui.editor_visual_radius_m << "\r\n";
    out << "min_screen_pct=" << ui.min_screen_size_pct << "\r\n";
    out << "sim_hour=" << ui.sim_hour << "\r\n";
    out << "sim_month=" << ui.sim_month << "\r\n";
    out << "sim_day=" << ui.sim_day << "\r\n";
    out << "sim_year=" << ui.sim_year << "\r\n";
    out << "time_running=" << (ui.time_running ? 1 : 0) << "\r\n";
    out << "time_speed=" << ui.time_speed << "\r\n";
    out << "chrono_date=" << ui.chrono_date << "\r\n";
    out << "chrono_map=" << ui.chrono_map << "\r\n";
    out << "timezone_h=" << ui.timezone_h << "\r\n";
    out << "weather_id=" << ui.weather_id << "\r\n";
    out << "show_sun_shadows=" << (ui.show_sun_shadows ? "1" : "0") << "\r\n";
    out << "shaft_plus=" << (ui.shaft_plus ? "1" : "0") << "\r\n";
    out << "fog_enabled=" << (ui.fog_enabled ? "1" : "0") << "\r\n";
    out << "parked_cars_on=" << (ui.parked_cars_on ? "1" : "0") << "\r\n";
    out << "show_onlyeditor_objects=" << (ui.show_onlyeditor_objects ? "1" : "0") << "\r\n";
    out << "shadow_distance_m=" << ui.shadow_distance_m << "\r\n";
    out << "vsync=" << (ui.vsync ? "1" : "0") << "\r\n";
    out << "world_sound_enabled=" << (ui.world_sound_enabled ? "1" : "0") << "\r\n";
    out << "world_sound_volume=" << ui.world_sound_volume << "\r\n";
    out << "shadow_map_size=" << ui.shadow_map_size << "\r\n";
    out << "path_style=" << ui.path_style << "\r\n";
    out << "hide_onlyeditor_splines=" << (ui.hide_onlyeditor_splines ? "1" : "0") << "\r\n";
    out << "language=" << ui.language << "\r\n";
    out << "show_sco_origin=" << (ui.show_sco_origin ? "1" : "0") << "\r\n";
    out << "arrows_3d=" << (ui.arrows_3d ? "1" : "0") << "\r\n";
    out << "hide_nodes=" << (ui.hide_nodes ? "1" : "0") << "\r\n";
    out << "union_ball_pct=" << ui.union_ball_screen_pct << "\r\n";
    out << "grad_circle_pct=" << ui.grad_circle_screen_pct << "\r\n";
    out << "tile_radius=" << ui.editor_tile_radius << "\r\n";
    out << "place_feedback=" << (ui.place_feedback ? "1" : "0") << "\r\n";
    out << "place_feedback_volume=" << ui.place_feedback_volume << "\r\n";
    out << "sfx_pack=" << ui.sfx_pack << "\r\n";
    out << "hints_ignore=" << (ui.hints_ignore ? "1" : "0") << "\r\n";
    out << "spline_stick_chain=" << (ui.spline_stick_chain ? "1" : "0") << "\r\n";
    out << "spline_fill_chain=" << (ui.spline_fill_chain ? "1" : "0") << "\r\n";
    // Positional [ui] keeps mode/speed too, but the key=value copy is what wins on load (parsed after [ui]).
    out << "terrain_brush_mode=" << ui.terrain_brush_mode << "\r\n";
    out << "terrain_brush_speed=" << ui.terrain_brush_speed << "\r\n";
    out << "\r\n[spawn_random]\r\n";
    out << "enabled=" << (ui.spawn_random ? "1" : "0") << "\r\n";
    out << "mode=" << ui.spawn_random_mode << "\r\n";
    out << "fav_cat=" << ui.spawn_random_fav_cat << "\r\n";
    out << "folder=" << ui.spawn_random_folder.string() << "\r\n";
    out << "\r\n[ui]\r\n";
    out << ui.tool << "\r\n";
    out << (ui.show_spline_ends ? "1" : "0") << "\r\n";
    out << (ui.show_path_ends ? "1" : "0") << "\r\n";
    out << (ui.show_path_rails ? "1" : "0") << "\r\n";
    out << (ui.autofill_spline_ends ? "1" : "0") << "\r\n";
    out << ui.terrain_brush_mode << "\r\n";
    out << ui.terrain_brush_size_m << "\r\n";
    out << ui.terrain_brush_speed << "\r\n";
    out << ui.terrain_set_height << "\r\n";
    out << ui.terrain_ground_layer << "\r\n";
    out << (ui.spawn_stick_terrain ? "1" : "0") << "\r\n";
    out << (ui.spawn_on_meshes ? "1" : "0") << "\r\n";
    out << ui.editor_tile_radius << "\r\n";
    out << (ui.spawn_align_pitch_bank ? "1" : "0") << "\r\n";
    out << ui.editor_visual_radius_m << "\r\n";
    out << "\r\n[menu_steps]\r\n";
    for (const auto& [key, step] : ui.field_steps) {
        if (key.empty() || !(step > 0.f)) continue;
        out << key << '=' << step << "\r\n";
    }
    out.flush();
    const bool ok = static_cast<bool>(out);
    out.close();
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec); // MoveFileEx(REPLACE_EXISTING) on MSVC
    if (ec) {
        // Fallback (e.g. AV holding the target): copy over it.
        ec.clear();
        omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
        std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        return !ec;
    }
    return true;
}

// The interface language until the user picks one: Spanish when Windows is in Spanish, English otherwise.
static int default_ui_language() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? 0 : 1;
}

EditorUiSettings parse_editor_ui_doc(const cfg::CfgDocument& doc) {
    EditorUiSettings s{};
    s.language = default_ui_language();
    if (doc.contains("[ui]") && !cfg::cfg_section(doc, "[ui]").empty()) {
        const auto& v = cfg::cfg_section(doc, "[ui]")[0];
        if (!v.empty()) {
            try {
                s.tool = std::stoi(v[0]);
            } catch (...) {
            }
        }
        if (v.size() > 1) s.show_spline_ends = parse_cfg_bool(v[1], s.show_spline_ends);
        if (v.size() > 2) s.show_path_ends = parse_cfg_bool(v[2], s.show_path_ends);
        if (v.size() > 3) s.show_path_rails = parse_cfg_bool(v[3], s.show_path_rails);
        if (v.size() > 4) s.autofill_spline_ends = parse_cfg_bool(v[4], s.autofill_spline_ends);
        if (v.size() > 5) {
            try {
                s.terrain_brush_mode = std::stoi(v[5]);
            } catch (...) {
            }
        }
        if (v.size() > 6) {
            try {
                s.terrain_brush_size_m = std::stof(v[6]);
            } catch (...) {
            }
        }
        if (v.size() > 7) {
            try {
                s.terrain_brush_speed = std::stof(v[7]);
            } catch (...) {
            }
        }
        if (v.size() > 8) {
            try {
                s.terrain_set_height = std::stof(v[8]);
            } catch (...) {
            }
        }
        if (v.size() > 9) {
            try {
                s.terrain_ground_layer = std::stoi(v[9]);
            } catch (...) {
            }
        }
        if (v.size() > 10) s.spawn_stick_terrain = parse_cfg_bool(v[10], s.spawn_stick_terrain);
        if (v.size() > 11) s.spawn_on_meshes = parse_cfg_bool(v[11], s.spawn_on_meshes);
        if (v.size() > 12) {
            try {
                s.editor_tile_radius = std::stoi(v[12]);
            } catch (...) {
            }
        }
        if (v.size() > 13) s.spawn_align_pitch_bank = parse_cfg_bool(v[13], s.spawn_align_pitch_bank);
        if (v.size() > 14) {
            try {
                s.editor_visual_radius_m = std::stof(v[14]);
            } catch (...) {
            }
        }
    }
    if (doc.contains("[vista]")) {
        for (const auto& block : cfg::cfg_section(doc, "[vista]")) {
            for (const auto& line : block) {
                const auto eq = line.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                apply_vista_kv(s, line.substr(0, eq), line.substr(eq + 1));
            }
        }
    }
    if (doc.contains("[spawn_random]")) {
        for (const auto& block : cfg::cfg_section(doc, "[spawn_random]")) {
            for (const auto& line : block) {
                const auto eq = line.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                const std::string key = line.substr(0, eq);
                const std::string val = line.substr(eq + 1);
                if (key == "enabled") s.spawn_random = parse_cfg_bool(val, s.spawn_random);
                else if (key == "mode") {
                    try {
                        const int m = std::stoi(val);
                        s.spawn_random_mode = (m < 0) ? 0 : ((m > 2) ? 2 : m);
                    } catch (...) {
                    }
                } else if (key == "fav_cat") {
                    try {
                        s.spawn_random_fav_cat = (std::max)(0, std::stoi(val));
                    } catch (...) {
                    }
                } else if (key == "folder") {
                    s.spawn_random_folder = val;
                }
            }
        }
    }
    if (doc.contains("[menu_steps]") && !cfg::cfg_section(doc, "[menu_steps]").empty()) {
        for (const auto& block : cfg::cfg_section(doc, "[menu_steps]")) {
            for (const auto& line : block) {
                const auto eq = line.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                const std::string key = line.substr(0, eq);
                try {
                    const float step = std::stof(line.substr(eq + 1));
                    if (step > 0.f) s.field_steps[key] = step;
                } catch (...) {
                }
            }
        }
    }
    if (s.spawn_stick_terrain && s.spawn_on_meshes) s.spawn_on_meshes = false;
    if (s.editor_tile_radius < 1) s.editor_tile_radius = 1;
    if (s.editor_tile_radius > 20) s.editor_tile_radius = 20;
    if (s.editor_visual_radius_m < 0.f) s.editor_visual_radius_m = 0.f;
    if (s.editor_visual_radius_m > 0.f && s.editor_visual_radius_m < 50.f) s.editor_visual_radius_m = 50.f;
    if (s.editor_visual_radius_m > 2000.f) s.editor_visual_radius_m = 2000.f;
    if (!(s.min_screen_size_pct >= 0.f)) s.min_screen_size_pct = 0.f;
    if (s.min_screen_size_pct > 30.f) s.min_screen_size_pct = 30.f;
    if (s.tool < 0) s.tool = 0;
    if (s.tool > 8) s.tool = 8;
    if (s.terrain_brush_mode < 0) s.terrain_brush_mode = 0;
    if (s.terrain_brush_mode > 5) s.terrain_brush_mode = 5; // 5 = FitMesh (ImGui only)
    if (!(s.terrain_brush_speed > 0.f)) s.terrain_brush_speed = 3.f;
    if (s.terrain_brush_speed > 10.f) s.terrain_brush_speed = 10.f;
    if (!(s.union_ball_screen_pct > 0.f)) s.union_ball_screen_pct = 0.02f;
    if (s.union_ball_screen_pct > 0.1f) s.union_ball_screen_pct = 0.1f;
    if (!(s.grad_circle_screen_pct > 0.f)) s.grad_circle_screen_pct = 0.01f;
    if (s.grad_circle_screen_pct > 0.1f) s.grad_circle_screen_pct = 0.1f;
    if (!(s.place_feedback_volume >= 0.f)) s.place_feedback_volume = 70.f;
    if (s.place_feedback_volume > 100.f) s.place_feedback_volume = 100.f;
    if (s.sim_month < 1 || s.sim_month > 12) s.sim_month = 6;
    if (s.sim_day < 1 || s.sim_day > 31) s.sim_day = 21;
    if (s.sim_year < 1900 || s.sim_year > 2200) s.sim_year = 2026;
    if (!(s.time_speed >= 1.f) || s.time_speed > 3600.f) s.time_speed = 1.f;
    if (!(s.sim_hour >= 0.f) || s.sim_hour >= 24.f) s.sim_hour = 14.f;
    if (s.weather_id < 0 || s.weather_id > 5) s.weather_id = 0;
    if (s.shadow_map_size < 512u || s.shadow_map_size > 4096u) s.shadow_map_size = 2048u;
    return s;
}

} // namespace

EditorUiSettings load_editor_ui_settings(const std::filesystem::path& map_root) {
    const auto global_path = options::editor_ui_prefs_path();
    if (auto doc = cfg::load_cfg_file(global_path)) {
        log::info("editor UI prefs loaded (global): " + global_path.string());
        return parse_editor_ui_doc(*doc);
    }
    // Migrate once from legacy per-map .omsi_f4_editor.cfg.
    if (!map_root.empty()) {
        const auto legacy = editor_cfg_resolve_path(map_root);
        if (auto doc = cfg::load_cfg_file(legacy)) {
            auto s = parse_editor_ui_doc(*doc);
            if (write_editor_ui_cfg(global_path, s)) {
                log::info("editor UI prefs migrated map→global: " + legacy.string() + " → " +
                          global_path.string());
            }
            return s;
        }
    }
    EditorUiSettings first_run{};
    first_run.language = default_ui_language();
    return first_run;
}

bool save_editor_geo_settings(const std::filesystem::path& map_root, const EditorGeoSettings& s) {
    bool ok = false;
    for (const auto& root : editor_cfg_write_roots(map_root)) {
        ok = write_editor_geo_cfg(root, s) || ok;
    }
    // Global copy (providers + API keys) shared by every map.
    if (const auto groot = editor_geo_global_root(); !groot.empty()) ok = write_editor_geo_cfg(groot, s) || ok;
    return ok;
}

bool save_editor_ui_settings(const std::filesystem::path& /*map_root*/, const EditorUiSettings& ui) {
    const auto path = options::editor_ui_prefs_path();
    const bool ok = write_editor_ui_cfg(path, ui);
    if (ok) log::info("editor UI prefs saved (global): " + path.string());
    else log::warn("editor UI prefs save failed: " + path.string());
    return ok;
}

bool write_terrain_heights(const std::filesystem::path& map_path, const std::vector<float>& heights) {
    if (omsi::vfs::write_blocked(map_path)) return false;
    const int n = 61 * 61;
    if (static_cast<int>(heights.size()) != n) return false;
    const auto path = std::filesystem::path(map_path.string() + ".terrain");
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::uint32_t header = 0;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(heights.data()), static_cast<std::streamsize>(heights.size() * sizeof(float)));
    return static_cast<bool>(out);
}

bool update_global_groundtex(const std::filesystem::path& global_cfg, const std::filesystem::path& diffuse_rel,
                             const float scale) {
    if (global_cfg.empty()) return false;
    auto lines = cfg::read_cfg_lines(global_cfg);
    if (lines.empty()) return false;

    const std::string rel = diffuse_rel.generic_string();
    bool in_section = false;
    bool updated = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (line == "[groundtex]") {
            in_section = true;
            continue;
        }
        if (in_section && !line.empty() && line.front() == '[') break;
        if (in_section && !line.empty()) {
            lines[i] = rel;
            updated = true;
            break;
        }
    }
    if (!updated) {
        lines.push_back("[groundtex]");
        lines.push_back(rel);
        lines.push_back("");
        lines.push_back("");
        lines.push_back(std::to_string(static_cast<int>(scale)));
    }

    omsi::fsutil::break_link(global_cfg); // copy-on-write workspace: never write through a link
    std::ofstream out(global_cfg, std::ios::trunc);
    if (!out) return false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\r\n";
    }
    return true;
}

bool append_global_groundtex(const std::filesystem::path& global_cfg, const std::filesystem::path& diffuse_rel,
                             const float scale) {
    if (global_cfg.empty()) return false;
    auto lines = cfg::read_cfg_lines(global_cfg);
    lines.push_back("[groundtex]");
    lines.push_back(diffuse_rel.generic_string());
    lines.push_back("");
    lines.push_back("");
    lines.push_back(std::to_string(static_cast<int>(scale)));
    omsi::fsutil::break_link(global_cfg); // copy-on-write workspace: never write through a link
    std::ofstream out(global_cfg, std::ios::trunc);
    if (!out) return false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\r\n";
    }
    return true;
}

bool write_global_groundtex(const std::filesystem::path& global_cfg, const int index, const std::string& main_tex,
                            const std::string& detail_tex, const int flag, const int main_rep, const int detail_rep) {
    if (global_cfg.empty()) return false;
    auto lines = cfg::read_cfg_lines(global_cfg);
    const std::vector<std::string> block = {"[groundtex]", main_tex, detail_tex, std::to_string(flag),
                                            std::to_string(main_rep), std::to_string(detail_rep)};
    bool done = false;
    if (index >= 0) {
        int found = -1;
        for (std::size_t i = 0; i < lines.size() && !done; ++i) {
            if (lines[i] != "[groundtex]") continue;
            if (++found != index) continue;
            std::size_t end = i + 1;
            while (end < lines.size() && (lines[end].empty() || lines[end].front() != '[')) ++end;
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i), lines.begin() + static_cast<std::ptrdiff_t>(end));
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i), block.begin(), block.end());
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i + block.size()), "");
            done = true;
        }
        if (!done) return false;
    } else {
        lines.push_back("");
        lines.insert(lines.end(), block.begin(), block.end());
        lines.push_back("");
    }
    omsi::fsutil::break_link(global_cfg); // copy-on-write workspace: never write through a link
    std::ofstream out(global_cfg, std::ios::trunc);
    if (!out) return false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\r\n";
    }
    return true;
}

bool delete_global_groundtex_at(const std::filesystem::path& global_cfg, const int index) {
    if (global_cfg.empty() || index < 0) return false;
    auto lines = cfg::read_cfg_lines(global_cfg);
    int found = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] != "[groundtex]") continue;
        ++found;
        if (found != index) continue;
        std::size_t end = i + 1;
        while (end < lines.size() && (lines[end].empty() || lines[end].front() != '[')) ++end;
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i),
                    lines.begin() + static_cast<std::ptrdiff_t>(end));
        omsi::fsutil::break_link(global_cfg); // copy-on-write workspace: never write through a link
        std::ofstream out(global_cfg, std::ios::trunc);
        if (!out) return false;
        for (std::size_t k = 0; k < lines.size(); ++k) {
            out << lines[k];
            if (k + 1 < lines.size()) out << "\r\n";
        }
        return true;
    }
    return false;
}

GeoJobResult download_tile_aerial_bmp(const std::string& api_key, const TileWgs84Bounds& bounds,
                                      const float tile_size_m, const std::filesystem::path& out_bmp) {
    GeoJobResult r{};
    if (api_key.empty()) {
        r.message = "API key vacia";
        return r;
    }

    const double lat_mid = (bounds.south + bounds.north) * 0.5;
    const int z = pick_zoom_for_tile(tile_size_m, lat_mid);
    const TileXY t0 = latlon_to_tile(bounds.south, bounds.west, z);
    const TileXY t1 = latlon_to_tile(bounds.north, bounds.east, z);
    const int x0 = (std::min)(t0.x, t1.x);
    const int x1 = (std::max)(t0.x, t1.x);
    const int y0 = (std::min)(t0.y, t1.y);
    const int y1 = (std::max)(t0.y, t1.y);

    const std::string session_body = R"({"mapType":"satellite","language":"en-US","region":"US"})";
    const std::wstring session_url = L"https://tile.googleapis.com/v1/createSession?key=" +
                                     std::wstring(api_key.begin(), api_key.end());
    const auto session_resp = http::http_post_json(session_url, session_body);
    if (session_resp.status != 200) {
        r.message = "createSession HTTP " + std::to_string(session_resp.status);
        return r;
    }
    const auto session = json_string_field(body_to_string(session_resp.body), "session");
    if (!session || session->empty()) {
        r.message = "createSession sin token (revisa billing/API Map Tiles)";
        return r;
    }

    const int tw = (x1 - x0 + 1) * 256;
    const int th = (y1 - y0 + 1) * 256;
    std::vector<std::uint8_t> mosaic(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 3, 40);

    int got = 0;
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            std::ostringstream q;
            q << "https://tile.googleapis.com/v1/2dtiles/" << z << "/" << tx << "/" << ty << "?session="
              << *session << "&key=" << api_key;
            const std::wstring url(q.str().begin(), q.str().end());
            const auto resp = http::http_get(url);
            if (resp.status != 200 || resp.body.empty()) continue;
            std::vector<std::uint8_t> rgb;
            UINT w = 0, h = 0;
            if (!decode_image_rgb(resp.body, rgb, w, h)) continue;
            blit_bgr(mosaic, tw, th, (tx - x0) * 256, (ty - y0) * 256, rgb, static_cast<int>(w), static_cast<int>(h));
            ++got;
        }
    }

    if (got == 0) {
        r.message = "No se descargaron tiles (API/quota)";
        return r;
    }

    std::vector<std::uint8_t> out512;
    resize_nearest_bgr(mosaic, tw, th, out512, 512, 512);
    std::filesystem::create_directories(out_bmp.parent_path());
    if (!write_bmp24(out_bmp, 512, 512, out512)) {
        r.message = "Error escribiendo BMP";
        return r;
    }

    r.ok = true;
    r.output_path = out_bmp;
    r.message = "Aerial OK z=" + std::to_string(z) + " tiles=" + std::to_string(got);
    return r;
}

GeoJobResult download_tile_dtm(const std::string& api_key, const TileWgs84Bounds& bounds, const int dim,
                               std::vector<float>& out_heights) {
    GeoJobResult r{};
    if (api_key.empty()) {
        r.message = "API key vacia";
        return r;
    }
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }

    struct Sample {
        double lat = 0.;
        double lon = 0.;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(dim * dim));
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const double lat = bounds.south + (bounds.north - bounds.south) * fy;
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const double lon = bounds.west + (bounds.east - bounds.west) * fx;
            samples.push_back({lat, lon});
        }
    }

    out_heights.assign(samples.size(), 0.f);
    constexpr std::size_t kBatch = 512;
    std::size_t idx = 0;
    while (idx < samples.size()) {
        const std::size_t end = (std::min)(idx + kBatch, samples.size());
        std::ostringstream q;
        q << "https://maps.googleapis.com/maps/api/elevation/json?locations=";
        for (std::size_t i = idx; i < end; ++i) {
            if (i > idx) q << '|';
            q << samples[i].lat << ',' << samples[i].lon;
        }
        q << "&key=" << api_key;
        const std::wstring url(q.str().begin(), q.str().end());
        const auto resp = http::http_get(url);
        if (resp.status != 200) {
            r.message = "Elevation HTTP " + std::to_string(resp.status);
            return r;
        }
        const auto vals = json_elevation_values(body_to_string(resp.body));
        if (vals.size() != end - idx) {
            r.message = "Elevation respuesta incompleta";
            return r;
        }
        for (std::size_t j = 0; j < vals.size(); ++j) out_heights[idx + j] = static_cast<float>(vals[j]);
        idx = end;
    }

    const float base = out_heights.front();
    for (auto& h : out_heights) h -= base;

    r.ok = true;
    r.message = "DTM OK puntos=" + std::to_string(out_heights.size()) + " base=" + std::to_string(base) + "m";
    return r;
}

GeoJobResult download_tile_aerial_google_static(const std::string& api_key, const TileWgs84Bounds& bounds,
                                                const int output_px, const std::filesystem::path& out_jpg) {
    GeoJobResult r{};
    if (api_key.empty()) {
        r.message = "Google Static Maps: API key vacia";
        return r;
    }
    if (!wgs84_bounds_sane(bounds)) {
        r.message = "Bounds WGS84 invalidos";
        return r;
    }
    const int scale = output_px > 640 ? 2 : 1;
    const int side = (std::min)(640, (output_px + scale - 1) / scale);
    std::ostringstream q;
    q << std::fixed;
    q << "https://maps.googleapis.com/maps/api/staticmap?maptype=satellite&scale=" << scale << "&size=" << side
      << "x" << side << "&visible=" << bounds.south << "," << bounds.west << "|" << bounds.north << "," << bounds.east
      << "&key=" << api_key;
    const std::wstring url(q.str().begin(), q.str().end());
    const auto resp = http::http_get(url);
    if (resp.status != 200 || resp.body.empty()) {
        r.message = "Google Static HTTP " + std::to_string(resp.status);
        return r;
    }
    std::vector<std::uint8_t> rgb;
    UINT w = 0, h = 0;
    if (!decode_image_rgb(resp.body, rgb, w, h)) {
        r.message = "Google Static: imagen invalida";
        return r;
    }
    std::vector<std::uint8_t> out_rgb;
    resize_nearest_bgr(rgb, static_cast<int>(w), static_cast<int>(h), out_rgb, output_px, output_px);
    std::filesystem::create_directories(out_jpg.parent_path());
    if (!write_jpeg_rgb(out_jpg, output_px, output_px, out_rgb)) {
        r.message = "Error escribiendo JPEG";
        return r;
    }
    r.ok = true;
    r.output_path = out_jpg;
    r.message = "Google Static Maps " + std::to_string(output_px) + "px scale=" + std::to_string(scale);
    return r;
}

namespace {

bool parse_esri_ascii_grid(const std::string& text, const int dim, std::vector<float>& out_heights) {
    std::istringstream in(text);
    std::string key;
    int ncols = 0;
    int nrows = 0;
    double nodata = -9999.0;
    while (in >> key) {
        if (key == "ncols") {
            in >> ncols;
        } else if (key == "nrows") {
            in >> nrows;
        } else if (key == "NODATA_value" || key == "nodata_value") {
            in >> nodata;
        } else if (key == "cellsize" || key == "xllcorner" || key == "yllcorner" || key == "xllcenter" ||
                   key == "yllcenter") {
            double skip = 0.;
            in >> skip;
        } else {
            break;
        }
    }
    if (ncols != dim || nrows != dim) return false;
    out_heights.assign(static_cast<std::size_t>(dim * dim), 0.f);
    for (int row = 0; row < dim; ++row) {
        for (int col = 0; col < dim; ++col) {
            double v = nodata;
            if (!(in >> v)) return false;
            if (std::fabs(v - nodata) < 1e-3 || std::isnan(v)) v = 0.0;
            out_heights[static_cast<std::size_t>(row) * static_cast<std::size_t>(dim) + static_cast<std::size_t>(col)] =
                static_cast<float>(v);
        }
    }
    return static_cast<bool>(in);
}

GeoJobResult download_opentopodata_dataset(const char* dataset, const TileWgs84Bounds& bounds, const int dim,
                                           std::vector<float>& out_heights) {
    GeoJobResult r{};
    struct Sample {
        double lat = 0.;
        double lon = 0.;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(dim * dim));
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const double lat = bounds.south + (bounds.north - bounds.south) * fy;
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const double lon = bounds.west + (bounds.east - bounds.west) * fx;
            samples.push_back({lat, lon});
        }
    }
    out_heights.assign(samples.size(), 0.f);
    std::size_t idx = 0;
    while (idx < samples.size()) {
        const std::size_t end = (std::min)(idx + static_cast<std::size_t>(100), samples.size());
        std::ostringstream q;
        q << "https://api.opentopodata.org/v1/" << dataset << "?locations=" << std::fixed;
        for (std::size_t i = idx; i < end; ++i) {
            if (i > idx) q << '|';
            q << samples[i].lat << ',' << samples[i].lon;
        }
        const std::wstring url(q.str().begin(), q.str().end());
        const auto resp = http::http_get(url);
        if (resp.status != 200) {
            r.message = std::string("OpenTopoData HTTP ") + std::to_string(resp.status);
            return r;
        }
        const auto vals = json_elevation_values(body_to_string(resp.body));
        if (vals.size() != end - idx) {
            r.message = "OpenTopoData respuesta incompleta";
            return r;
        }
        for (std::size_t j = 0; j < vals.size(); ++j) out_heights[idx + j] = static_cast<float>(vals[j]);
        idx = end;
        if (idx < samples.size()) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
    fill_nan_heights(out_heights, dim);
    float base = out_heights.front();
    for (float h : out_heights)
        if (!std::isnan(h)) {
            base = h;
            break;
        }
    for (auto& h : out_heights) h -= base;
    r.ok = true;
    r.message = std::string("OpenTopoData ") + dataset + " OK base=" + std::to_string(base) + "m";
    return r;
}

} // namespace

GeoJobResult download_tile_dtm_srtm3(const TileWgs84Bounds& bounds, const int dim, std::vector<float>& out_heights) {
    if (dim < 2) {
        GeoJobResult r{};
        r.message = "dim invalida";
        return r;
    }
    return download_opentopodata_dataset("srtm30m", bounds, dim, out_heights);
}

GeoJobResult download_tile_dtm_geoportal_pl(const TileWgs84Bounds& bounds, const int dim,
                                            std::vector<float>& out_heights) {
    GeoJobResult r{};
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }
    if (!bounds_in_poland(bounds)) {
        r.message = "Geoportal NMT: solo Polonia (lat 49-55, lon 14-24.5)";
        return r;
    }
    const char* coverages[] = {"NMT-PL-EVRF2007-NH", "NMT-PL-KRON86-NH"};
    for (const char* coverage : coverages) {
        std::ostringstream q;
        q << std::fixed;
        q << "https://mapy.geoportal.gov.pl/wss/service/PZGIK/NMT/GRID1/WCS/DigitalTerrainModel"
          << "?SERVICE=WCS&VERSION=1.0.0&REQUEST=GetCoverage&COVERAGE=" << coverage << "&CRS=EPSG:4326&BBOX="
          << bounds.west << "," << bounds.south << "," << bounds.east << "," << bounds.north << "&WIDTH=" << dim
          << "&HEIGHT=" << dim << "&FORMAT=ArcGrid";
        const std::wstring url(q.str().begin(), q.str().end());
        const auto resp = http::http_get(url);
        if (resp.status != 200 || resp.body.empty()) continue;
        std::vector<float> parsed;
        if (!parse_esri_ascii_grid(body_to_string(resp.body), dim, parsed)) continue;
        out_heights = std::move(parsed);
        const float base = out_heights.front();
        for (auto& h : out_heights) h -= base;
        r.ok = true;
        r.message = std::string("Geoportal NMT WCS ") + coverage + " base=" + std::to_string(base) + "m";
        return r;
    }
    r.message = "Geoportal NMT WCS fallo — prueba OpenTopoData fuera de PL";
    return r;
}

GeoJobResult download_tile_dtm_terrarium(const TileWgs84Bounds& bounds, const int dim,
                                         std::vector<float>& out_heights) {
    GeoJobResult r{};
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoScope {
        bool active = false;
        ~CoScope() {
            if (active) CoUninitialize();
        }
    } co_scope;
    co_scope.active = SUCCEEDED(co_hr);
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }
    // ~30 m DEM ≈ z15 for a 300–600 m OMSI tile (z14 is ~5 m/px at Berlin).
    int z = 15;
    for (; z < 16; ++z) {
        const double span = std::abs(mercator_pixel_x(bounds.east, z) - mercator_pixel_x(bounds.west, z));
        if (span >= 256.0) break;
    }
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (;;) {
        const TileXY t0 = latlon_to_tile(bounds.south, bounds.west, z);
        const TileXY t1 = latlon_to_tile(bounds.north, bounds.east, z);
        x0 = (std::min)(t0.x, t1.x);
        x1 = (std::max)(t0.x, t1.x);
        y0 = (std::min)(t0.y, t1.y);
        y1 = (std::max)(t0.y, t1.y);
        const int nx = x1 - x0 + 1;
        const int ny = y1 - y0 + 1;
        if (nx > 0 && ny > 0 && static_cast<std::int64_t>(nx) * ny <= 64) break;
        if (z <= 10) break;
        --z;
    }
    const int tw = (x1 - x0 + 1) * 256;
    const int th = (y1 - y0 + 1) * 256;
    std::vector<std::uint8_t> mosaic(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 3, 0);
    int got = 0;
    const char* hosts[] = {"https://elevation-tiles-prod.s3.amazonaws.com/terrarium/",
                           "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/"};
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            bool tile_ok = false;
            for (const char* host : hosts) {
                std::ostringstream q;
                q << host << z << '/' << tx << '/' << ty << ".png";
                const std::string url8 = q.str();
                const std::wstring url(url8.begin(), url8.end());
                const auto resp = http::http_get(url);
                if (resp.status != 200 || resp.body.empty()) continue;
                std::vector<std::uint8_t> rgb;
                UINT w = 0, h = 0;
                if (!decode_image_rgb(resp.body, rgb, w, h)) continue;
                blit_bgr(mosaic, tw, th, (tx - x0) * 256, (ty - y0) * 256, rgb, static_cast<int>(w),
                         static_cast<int>(h));
                ++got;
                tile_ok = true;
                break;
            }
            (void)tile_ok;
        }
    }
    if (got == 0) {
        r.message = "Terrarium: sin tiles (red / S3)";
        return r;
    }
    std::vector<std::uint8_t> cropped;
    int crop_w = tw, crop_h = th;
    if (!crop_mosaic_to_bounds(mosaic, tw, th, x0, y0, z, bounds, cropped, crop_w, crop_h, 256)) {
        cropped = mosaic;
        crop_w = tw;
        crop_h = th;
    }
    out_heights.assign(static_cast<std::size_t>(dim * dim), 0.f);
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const int sy = (std::min)(crop_h - 1, static_cast<int>(fy * (crop_h - 1) + 0.5));
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const int sx = (std::min)(crop_w - 1, static_cast<int>(fx * (crop_w - 1) + 0.5));
            const std::size_t i = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(crop_w) +
                                   static_cast<std::size_t>(sx)) *
                                  3u;
            // WIC decode is BGR: B,G,R. Terrarium: h = R*256 + G + B/256 - 32768.
            const float B = static_cast<float>(cropped[i + 0]);
            const float G = static_cast<float>(cropped[i + 1]);
            const float R = static_cast<float>(cropped[i + 2]);
            out_heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) +
                        static_cast<std::size_t>(x)] = R * 256.f + G + B / 256.f - 32768.f;
        }
    }
    r.ok = true;
    r.message = "Terrarium z=" + std::to_string(z) + " tiles=" + std::to_string(got) +
                " z0=" + std::to_string(out_heights.front()) + "m";
    return r;
}

GeoJobResult download_tile_dtm_mapterhorn(const TileWgs84Bounds& bounds, const int dim,
                                          std::vector<float>& out_heights) {
    // Same Terrarium decode as AWS; tiles are 512² WebP from tiles.mapterhorn.com.
    GeoJobResult r{};
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoScope {
        bool active = false;
        ~CoScope() {
            if (active) CoUninitialize();
        }
    } co_scope;
    co_scope.active = SUCCEEDED(co_hr);
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }
    constexpr int kTile = 512;
    int z = 14;
    for (; z < 16; ++z) {
        const double span = std::abs(mercator_pixel_x(bounds.east, z) - mercator_pixel_x(bounds.west, z));
        if (span >= static_cast<double>(kTile)) break;
    }
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (;;) {
        const TileXY t0 = latlon_to_tile(bounds.south, bounds.west, z);
        const TileXY t1 = latlon_to_tile(bounds.north, bounds.east, z);
        x0 = (std::min)(t0.x, t1.x);
        x1 = (std::max)(t0.x, t1.x);
        y0 = (std::min)(t0.y, t1.y);
        y1 = (std::max)(t0.y, t1.y);
        const int nx = x1 - x0 + 1;
        const int ny = y1 - y0 + 1;
        if (nx > 0 && ny > 0 && static_cast<std::int64_t>(nx) * ny <= 36) break;
        if (z <= 10) break;
        --z;
    }
    const int tw = (x1 - x0 + 1) * kTile;
    const int th = (y1 - y0 + 1) * kTile;
    std::vector<std::uint8_t> mosaic(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 3, 0);
    int got = 0;
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            std::ostringstream q;
            q << "https://tiles.mapterhorn.com/" << z << '/' << tx << '/' << ty << ".webp";
            const std::string url8 = q.str();
            const std::wstring url(url8.begin(), url8.end());
            const auto resp = http::http_get(url);
            if (resp.status != 200 || resp.body.empty()) continue;
            std::vector<std::uint8_t> rgb;
            UINT w = 0, h = 0;
            if (!decode_image_rgb(resp.body, rgb, w, h)) continue;
            blit_bgr(mosaic, tw, th, (tx - x0) * kTile, (ty - y0) * kTile, rgb, static_cast<int>(w),
                     static_cast<int>(h));
            ++got;
        }
    }
    if (got == 0) {
        r.message = "Mapterhorn: sin tiles (red / webp)";
        return r;
    }
    std::vector<std::uint8_t> cropped;
    int crop_w = tw, crop_h = th;
    if (!crop_mosaic_to_bounds(mosaic, tw, th, x0, y0, z, bounds, cropped, crop_w, crop_h, kTile)) {
        cropped = mosaic;
        crop_w = tw;
        crop_h = th;
    }
    out_heights.assign(static_cast<std::size_t>(dim * dim), 0.f);
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const int sy = (std::min)(crop_h - 1, static_cast<int>(fy * (crop_h - 1) + 0.5));
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const int sx = (std::min)(crop_w - 1, static_cast<int>(fx * (crop_w - 1) + 0.5));
            const std::size_t i = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(crop_w) +
                                   static_cast<std::size_t>(sx)) *
                                  3u;
            const float B = static_cast<float>(cropped[i + 0]);
            const float G = static_cast<float>(cropped[i + 1]);
            const float R = static_cast<float>(cropped[i + 2]);
            out_heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) +
                        static_cast<std::size_t>(x)] = R * 256.f + G + B / 256.f - 32768.f;
        }
    }
    r.ok = true;
    r.message = "Mapterhorn z=" + std::to_string(z) + " tiles=" + std::to_string(got) +
                " z0=" + std::to_string(out_heights.front()) + "m";
    return r;
}

GeoJobResult download_tile_dtm_openmeteo(const TileWgs84Bounds& bounds, const int dim,
                                         std::vector<float>& out_heights) {
    GeoJobResult r{};
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }
    if (!wgs84_bounds_sane(bounds)) {
        r.message = "Bounds WGS84 invalidos — revisa ref lat/lon";
        return r;
    }
    struct Sample {
        double lat = 0.;
        double lon = 0.;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(dim * dim));
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const double lat = bounds.south + (bounds.north - bounds.south) * fy;
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const double lon = bounds.west + (bounds.east - bounds.west) * fx;
            samples.push_back({lat, lon});
        }
    }
    out_heights.assign(samples.size(), 0.f);
    constexpr std::size_t kBatch = 80;
    struct ElevBatch {
        std::size_t idx = 0;
        std::size_t end = 0;
        std::vector<double> vals;
        int status = 0;
        bool ok = false;
    };
    std::vector<ElevBatch> batches;
    for (std::size_t idx = 0; idx < samples.size(); idx += kBatch) {
        ElevBatch b;
        b.idx = idx;
        b.end = (std::min)(idx + kBatch, samples.size());
        batches.push_back(std::move(b));
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n_workers =
        (std::min)(16u, (std::max)(4u, hw == 0 ? 8u : hw * 2u));
    std::atomic<std::size_t> next_job{0};
    std::vector<std::thread> pool;
    pool.reserve(n_workers);
    for (unsigned wi = 0; wi < n_workers; ++wi) {
        pool.emplace_back([&]() {
            for (;;) {
                const std::size_t bi = next_job.fetch_add(1, std::memory_order_relaxed);
                if (bi >= batches.size()) break;
                auto& b = batches[bi];
                std::ostringstream latq;
                std::ostringstream lonq;
                latq << std::fixed;
                lonq << std::fixed;
                for (std::size_t i = b.idx; i < b.end; ++i) {
                    if (i > b.idx) {
                        latq << ',';
                        lonq << ',';
                    }
                    latq << samples[i].lat;
                    lonq << samples[i].lon;
                }
                std::ostringstream q;
                q << "https://api.open-meteo.com/v1/elevation?latitude=" << latq.str()
                  << "&longitude=" << lonq.str();
                const std::string url8 = q.str();
                const std::wstring url(url8.begin(), url8.end());
                const auto resp = http::http_get(url);
                b.status = resp.status;
                if (resp.status == 200) {
                    b.vals = json_elevation_array(body_to_string(resp.body));
                    b.ok = (b.vals.size() == b.end - b.idx);
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    for (const auto& b : batches) {
        if (!b.ok) {
            r.message = b.status != 200 ? ("Open-Meteo HTTP " + std::to_string(b.status))
                                        : "Open-Meteo respuesta incompleta";
            return r;
        }
        for (std::size_t j = 0; j < b.vals.size(); ++j)
            out_heights[b.idx + j] = static_cast<float>(b.vals[j]);
    }
    fill_nan_heights(out_heights, dim);
    r.ok = true;
    r.message = "Open-Meteo GLO-90 OK puntos=" + std::to_string(out_heights.size()) +
                " z0=" + std::to_string(out_heights.front()) + "m parallel=" + std::to_string(n_workers);
    return r;
}

GeoJobResult download_tile_dtm_open_elevation(const TileWgs84Bounds& bounds, const int dim,
                                              std::vector<float>& out_heights) {
    GeoJobResult r{};
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }
    if (!wgs84_bounds_sane(bounds)) {
        r.message = "Bounds WGS84 invalidos — revisa ref lat/lon";
        return r;
    }
    struct Sample {
        double lat = 0.;
        double lon = 0.;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(dim * dim));
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const double lat = bounds.south + (bounds.north - bounds.south) * fy;
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const double lon = bounds.west + (bounds.east - bounds.west) * fx;
            samples.push_back({lat, lon});
        }
    }
    out_heights.assign(samples.size(), 0.f);
    constexpr std::size_t kBatch = 80;
    struct ElevBatch {
        std::size_t idx = 0;
        std::size_t end = 0;
        std::vector<double> vals;
        int status = 0;
        bool ok = false;
    };
    std::vector<ElevBatch> batches;
    for (std::size_t idx = 0; idx < samples.size(); idx += kBatch) {
        ElevBatch b;
        b.idx = idx;
        b.end = (std::min)(idx + kBatch, samples.size());
        batches.push_back(std::move(b));
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n_workers =
        (std::min)(12u, (std::max)(4u, hw == 0 ? 6u : hw));
    std::atomic<std::size_t> next_job{0};
    std::vector<std::thread> pool;
    pool.reserve(n_workers);
    for (unsigned wi = 0; wi < n_workers; ++wi) {
        pool.emplace_back([&]() {
            for (;;) {
                const std::size_t bi = next_job.fetch_add(1, std::memory_order_relaxed);
                if (bi >= batches.size()) break;
                auto& b = batches[bi];
                std::ostringstream body;
                body << std::fixed << "{\"locations\":[";
                for (std::size_t i = b.idx; i < b.end; ++i) {
                    if (i > b.idx) body << ',';
                    body << "{\"latitude\":" << samples[i].lat << ",\"longitude\":" << samples[i].lon << '}';
                }
                body << "]}";
                const auto resp = http::http_post_json(L"https://api.open-elevation.com/api/v1/lookup", body.str());
                b.status = resp.status;
                if (resp.status == 200) {
                    b.vals = json_elevation_values(body_to_string(resp.body));
                    b.ok = (b.vals.size() == b.end - b.idx);
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    for (const auto& b : batches) {
        if (!b.ok) {
            r.message = b.status != 200 ? ("Open-Elevation HTTP " + std::to_string(b.status))
                                        : "Open-Elevation respuesta incompleta";
            return r;
        }
        for (std::size_t j = 0; j < b.vals.size(); ++j)
            out_heights[b.idx + j] = static_cast<float>(b.vals[j]);
    }
    fill_nan_heights(out_heights, dim);
    r.ok = true;
    r.message = "Open-Elevation OK puntos=" + std::to_string(out_heights.size()) +
                " z0=" + std::to_string(out_heights.front()) + "m parallel=" + std::to_string(n_workers);
    return r;
}

GeoJobResult download_tile_dtm_with_fallback(const ElevationProvider provider, const std::string& google_key,
                                             const TileWgs84Bounds& bounds, const int dim,
                                             std::vector<float>& out_heights) {
    auto try_one = [&](ElevationProvider p) -> GeoJobResult {
        switch (p) {
        case ElevationProvider::OpenTopoData:
            return download_tile_dtm_opentopodata(bounds, dim, out_heights);
        case ElevationProvider::NasaSrtm3:
            return download_tile_dtm_srtm3(bounds, dim, out_heights);
        case ElevationProvider::GeoportalPlNmt:
            return download_tile_dtm_geoportal_pl(bounds, dim, out_heights);
        case ElevationProvider::Terrarium:
            return download_tile_dtm_terrarium(bounds, dim, out_heights);
        case ElevationProvider::Mapterhorn:
            return download_tile_dtm_mapterhorn(bounds, dim, out_heights);
        case ElevationProvider::OpenMeteo:
            return download_tile_dtm_openmeteo(bounds, dim, out_heights);
        case ElevationProvider::OpenElevation:
            return download_tile_dtm_open_elevation(bounds, dim, out_heights);
        case ElevationProvider::CopernicusGlo30:
            return download_tile_dtm_copernicus_glo30(bounds, dim, out_heights);
        case ElevationProvider::Fabdem:
            return download_tile_dtm_fabdem(bounds, dim, out_heights);
        case ElevationProvider::Google:
            if (google_key.empty()) {
                GeoJobResult miss{};
                miss.message = "Google Elevation: sin API key";
                return miss;
            }
            return download_tile_dtm(google_key, bounds, dim, out_heights);
        }
        GeoJobResult unk{};
        unk.message = "proveedor DTM desconocido";
        return unk;
    };

    GeoJobResult r = try_one(provider);
    if (r.ok) return r;
    const std::string first = r.message;
    const ElevationProvider chain[] = {ElevationProvider::Fabdem, ElevationProvider::CopernicusGlo30,
                                       ElevationProvider::OpenMeteo, ElevationProvider::OpenTopoData,
                                       ElevationProvider::Terrarium, ElevationProvider::Mapterhorn,
                                       ElevationProvider::OpenElevation};
    for (const auto p : chain) {
        if (p == provider) continue;
        r = try_one(p);
        if (r.ok) {
            r.message += " (fallback; " + first + ")";
            return r;
        }
    }
    r.message = first + " | fallbacks Open-Meteo/Open-Elevation/OpenTopo/Terrarium fallaron";
    return r;
}

GeoJobResult import_heightmap_file(const std::filesystem::path& path, const int dim,
                                   std::vector<float>& out_heights) {
    GeoJobResult r{};
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoScope {
        bool active = false;
        ~CoScope() {
            if (active) CoUninitialize();
        }
    } co_scope;
    co_scope.active = SUCCEEDED(co_hr);
    if (dim < 2 || path.empty()) {
        r.message = "path/dim invalida";
        return r;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        r.message = "no se pudo abrir " + path.filename().string();
        return r;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        r.message = "archivo vacio";
        return r;
    }
    std::vector<std::uint8_t> rgb;
    UINT w = 0, h = 0;
    if (!decode_image_rgb(bytes, rgb, w, h) || w < 2 || h < 2) {
        r.message = "WIC no pudo leer DEM (TIFF/PNG/BMP)";
        return r;
    }
    out_heights.assign(static_cast<std::size_t>(dim * dim), 0.f);
    // Detect Terrarium-looking range vs plain grayscale luminance.
    bool looks_terrarium = false;
    {
        float min_h = 1.e9f, max_h = -1.e9f;
        for (UINT i = 0; i < (std::min)(w * h, 64u); ++i) {
            const float B = rgb[i * 3 + 0], G = rgb[i * 3 + 1], R = rgb[i * 3 + 2];
            const float th = R * 256.f + G + B / 256.f - 32768.f;
            min_h = (std::min)(min_h, th);
            max_h = (std::max)(max_h, th);
        }
        looks_terrarium = max_h - min_h > 5.f && min_h > -500.f && max_h < 9000.f &&
                          (rgb[2] != rgb[1] || rgb[1] != rgb[0]);
    }
    for (int y = 0; y < dim; ++y) {
        const int sy = (std::min)(static_cast<int>(h) - 1,
                                  static_cast<int>(static_cast<double>(y) / (dim - 1) * (h - 1) + 0.5));
        for (int x = 0; x < dim; ++x) {
            const int sx = (std::min)(static_cast<int>(w) - 1,
                                      static_cast<int>(static_cast<double>(x) / (dim - 1) * (w - 1) + 0.5));
            const std::size_t i =
                (static_cast<std::size_t>(sy) * w + static_cast<std::size_t>(sx)) * 3u;
            float hv = 0.f;
            if (looks_terrarium) {
                const float B = rgb[i], G = rgb[i + 1], R = rgb[i + 2];
                hv = R * 256.f + G + B / 256.f - 32768.f;
            } else {
                hv = (0.114f * rgb[i] + 0.587f * rgb[i + 1] + 0.299f * rgb[i + 2]) / 255.f * 100.f;
            }
            out_heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) +
                        static_cast<std::size_t>(x)] = hv;
        }
    }
    float base = out_heights.front();
    for (auto& hv : out_heights) hv -= base;
    r.ok = true;
    r.message = std::string("DEM import ") + path.filename().string() + " " + std::to_string(w) + "x" +
                std::to_string(h) + (looks_terrarium ? " (terrarium)" : " (luma→m)");
    return r;
}
