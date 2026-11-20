#pragma once

// Sun + sky: OpenOMSI `daylight.rs` (solar altitude, twilight, sky_weights, night)
// plus OMSI THimmel nightmaps when lamps/NightlightA > 0.5.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace omsi::sky {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kObliquity = 0.40910517666747087f; // kept for TGestirn helpers
// envir.cfg default [twilight_start_end] (OpenOMSI / stock OMSI).
inline constexpr float kTwilightStartDeg = -18.f;
inline constexpr float kTwilightEndDeg = 10.f;

struct SkyState {
    float azimuth_rad = 0.f;
    float elevation_rad = 0.f;
    float elevation_deg = 0.f;
    // Engine Z-up (OpenOMSI: x east, y north, z up).
    float sun_x = 0.40f;
    float sun_y = 0.22f;
    float sun_z = 0.88f;
    float ambient_scale = 1.f;
    float night_blend = 0.f; // 0 day … 1 night (matl_nightmap / texture\night)
    float sky_r = 0.69f;
    float sky_g = 0.77f;
    float sky_b = 0.87f;
    float cloud_density = 0.22f;
    float sky_w_day = 1.f;
    float sky_w_dusk = 0.f;
    float sky_w_night = 0.f;
    const char* season_dir = "SummerDry"; // TTextureMan extra folder (_Unit130)
    bool prefer_night = false;
    float lst_rad = 0.f;   // local sidereal time (stars rotate with the sun), radians
    float lat_rad = 0.f;   // observer latitude, radians
};

// OMSI weather presets. Values read from the stock Weather\*.owt files (fog range/visibility,
// [clouds] type + base altitude, [precip] type, [wind] direction/speed). Ids match the UI slider.
enum class CloudType : std::uint8_t { None, Cumulus1, Cumulus3, Overcast1, Overcast2 };
struct WeatherPreset {
    const char* name;
    CloudType cloud_type;
    float cover;          // 0..1 (scattered ~0.45, overcast ~0.95)
    float cloud_base_m;   // cloud layer base above the map zero plane
    float fog_range_m;    // OMSI [fog] range (large = clear)
    int precip;           // 0 none, 1 rain, 2 snow
    float precip_intensity; // 0..1
    float wind_from_deg;  // OMSI [wind] direction (wind comes FROM)
    float wind_mps;       // OMSI [wind] speed
};
inline const WeatherPreset& weather_preset(int id) {
    static constexpr WeatherPreset kPresets[] = {
        {"CAVOK (clear)", CloudType::None, 0.f, 0.f, 50000.f, 0, 0.f, 0.f, 0.f},
        {"Sommerlich", CloudType::Cumulus1, 0.45f, 300.f, 2000.f, 0, 0.f, 70.f, 0.84f},
        {"Schmuddelwetter (rain)", CloudType::Overcast1, 0.95f, 200.f, 200.f, 1, 0.6f, 230.f, 4.34f},
        {"Starker Schneefall (snow)", CloudType::Cumulus3, 0.55f, 300.f, 50000.f, 2, 0.8f, 209.f, 0.84f},
        {"Bodennebel (ground fog)", CloudType::None, 0.f, 0.f, 75.f, 0, 0.f, 0.f, 0.f},
        {"Heavy Rain", CloudType::Overcast2, 0.98f, 3000.f, 75.f, 1, 1.0f, 230.f, 4.34f},
    };
    const int n = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets[(std::max)(0, (std::min)(id, n - 1))];
}

inline int clamp_month(int m) { return (std::max)(1, (std::min)(m, 12)); }
inline int clamp_day(int d) { return (std::max)(1, (std::min)(d, 31)); }

inline int day_of_year(int month, int day) {
    static constexpr int kCum[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    month = clamp_month(month);
    day = clamp_day(day);
    int n = kCum[month] + day;
    if (n < 1) n = 1;
    if (n > 365) n = 365;
    return n;
}

inline void month_day_from_doy(int doy, int& month, int& day) {
    static constexpr int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
    doy = (std::max)(1, (std::min)(doy, 365));
    month = 1;
    for (int m = 1; m <= 12; ++m) {
        if (doy <= kCum[m]) {
            month = m;
            day = doy - kCum[m - 1];
            return;
        }
    }
    month = 12;
    day = 31;
}

inline const char* season_folder_for_month(int month, int weather_id) {
    if (weather_id == 3) return "WinterSnow";
    month = clamp_month(month);
    if (month == 12 || month <= 2) return "Winter";
    if (month <= 5) return "Spring";
    if (month <= 8) return "SummerDry";
    return "Fall";
}

// TGestirn.Calculate(stundenwinkel_ort) with latitude degrees (legacy; evaluate uses OpenOMSI).
inline void gestirn_calculate(float hour_angle_rad, float decl_rad, float lat_deg, float& az, float& el, float& px,
                              float& py, float& pz) {
    const float lat = lat_deg * kPi / 180.f;
    const float ha = hour_angle_rad;
    const float cos_ha = std::cos(ha);
    const float sin_lat = std::sin(lat);
    const float cos_lat = std::cos(lat);
    const float tan_dec = std::tan(decl_rad);
    float denom = cos_lat * tan_dec - sin_lat * cos_ha;
    az = std::atan(std::sin(ha) / (denom == 0.f ? 1e-8f : denom));
    if (denom < 0.f) az += kPi;
    el = std::asin(std::sin(decl_rad) * sin_lat + std::cos(decl_rad) * cos_lat * cos_ha);
    const float ce = std::cos(el);
    px = -std::sin(az) * ce;
    py = std::sin(el);
    pz = -std::cos(az) * ce;
}

// OpenOMSI sun_position + Daylight::compute (clock, lat/lon, timezone, twilight).
inline SkyState evaluate(int doy, float hour_of_day, float lat_deg, float lon_deg, float timezone_h, int weather_id) {
    SkyState s{};
    doy = (std::max)(1, (std::min)(doy, 365));
    hour_of_day = std::fmod(hour_of_day, 24.f);
    if (hour_of_day < 0.f) hour_of_day += 24.f;
    if (lat_deg == 0.f && lon_deg == 0.f) {
        lat_deg = 52.505f;
        lon_deg = 13.2782f;
    }

    int month = 6, mday = 1;
    month_day_from_doy(doy, month, mday);
    s.season_dir = season_folder_for_month(month, weather_id);

    const float tw0 = kTwilightStartDeg;
    const float tw1 = kTwilightEndDeg;
    const float decl = (23.44f * kPi / 180.f) *
                       std::sin(2.f * kPi * (284.f + static_cast<float>(doy)) / 365.f);
    const float solar_h = hour_of_day - timezone_h + lon_deg / 15.f;
    const float ha = (15.f * (solar_h - 12.f)) * kPi / 180.f;
    const float lat = lat_deg * kPi / 180.f;
    const float sin_alt = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(ha);
    const float alt_rad = std::asin((std::max)(-1.f, (std::min)(1.f, sin_alt)));
    const float cos_az = (std::sin(decl) - std::sin(lat) * sin_alt) / (std::max)(1e-6f, std::cos(lat) * std::cos(alt_rad));
    float az = std::acos((std::max)(-1.f, (std::min)(1.f, cos_az)));
    if (std::sin(ha) > 0.f) az = 2.f * kPi - az;

    const float alt = alt_rad * 180.f / kPi;
    const float ca = std::cos(alt_rad);
    const float sa = std::sin(alt_rad);
    const float saz = std::sin(az);
    const float caz = std::cos(az);
    s.azimuth_rad = az;
    s.elevation_rad = alt_rad;
    s.elevation_deg = alt;
    s.sun_x = ca * saz;
    s.sun_y = ca * caz;
    s.sun_z = sa;
    const float len = std::sqrt(s.sun_x * s.sun_x + s.sun_y * s.sun_y + s.sun_z * s.sun_z);
    if (len > 1e-4f) {
        s.sun_x /= len;
        s.sun_y /= len;
        s.sun_z /= len;
    }

    float solar_night = (tw1 - alt) / (std::max)(tw1, 1.f);
    solar_night = (std::max)(0.f, (std::min)(1.f, solar_night));

    float clock_n = 0.f;
    if (hour_of_day < 5.5f || hour_of_day >= 21.5f) clock_n = 1.f;
    else if (hour_of_day < 7.f) clock_n = (7.f - hour_of_day) / 1.5f;
    else if (hour_of_day >= 20.f) clock_n = (hour_of_day - 20.f) / 1.5f;

    s.night_blend = (std::max)(solar_night, clock_n);
    s.prefer_night = s.night_blend > 0.5f;

    // Sun ecliptic longitude -> right ascension (approx., 2026 epoch); LST = RA_sun + sun hour angle.
    {
        const float n_days = 9496.5f + static_cast<float>(doy - 1) + hour_of_day / 24.f;
        const float mean_lon = 280.46f + 0.9856474f * n_days;
        const float g = (357.528f + 0.9856003f * n_days) * kPi / 180.f;
        const float lam = (mean_lon + 1.915f * std::sin(g)) * kPi / 180.f;
        const float eps = 23.439f * kPi / 180.f;
        float ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam));
        float lst = ra + ha;
        lst = std::fmod(lst, 2.f * kPi);
        if (lst < 0.f) lst += 2.f * kPi;
        s.lst_rad = lst;
        s.lat_rad = lat;
    }

    if (alt >= tw1) {
        s.sky_w_day = 1.f;
        s.sky_w_dusk = 0.f;
        s.sky_w_night = 0.f;
    } else if (alt >= 0.f) {
        const float t = alt / (std::max)(tw1, 0.1f);
        s.sky_w_day = t;
        s.sky_w_dusk = 1.f - t;
        s.sky_w_night = 0.f;
    } else if (alt >= tw0) {
        const float t = (alt - tw0) / (std::max)(0.f - tw0, 0.1f);
        s.sky_w_day = 0.f;
        s.sky_w_dusk = t;
        s.sky_w_night = 1.f - t;
    } else {
        s.sky_w_day = 0.f;
        s.sky_w_dusk = 0.f;
        s.sky_w_night = 1.f;
    }
    if (clock_n > 0.85f) {
        s.sky_w_day = 0.f;
        s.sky_w_dusk = 0.f;
        s.sky_w_night = 1.f;
    }

    float brightness = (std::max)(0.f, (std::min)(1.f, (alt + 6.f) / 12.f));
    brightness *= (1.f - clock_n);
    const WeatherPreset& wp = weather_preset(weather_id);
    brightness *= 1.f - 0.45f * wp.cover;
    if (wp.precip != 0) brightness *= 0.8f;
    // Day ambient ~0.28–0.35; night floor ~0.10 so unlit faces read dark without going black.
    s.ambient_scale = (std::max)(0.10f, 0.28f + brightness * 0.45f);
    if (clock_n > 0.5f) s.ambient_scale = (std::max)(0.10f, brightness * 0.55f + 0.08f);

    const float day = (std::max)(0.f, (std::min)(1.f, (alt + 6.f) / 16.f)) * (1.f - clock_n);
    s.sky_r = 0.55f * day + 0.01f * (1.f - day);
    s.sky_g = 0.70f * day + 0.01f * (1.f - day);
    s.sky_b = 0.92f * day + 0.03f * (1.f - day);

    s.cloud_density = wp.cover;

    return s;
}

} // namespace omsi::sky
