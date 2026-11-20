#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"
#include "omsi/map_geo_tools.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/http_client.hpp"
#include "omsi/logger.hpp"
#include "omsi/options.hpp"
#include "omsi/utf8_win32.hpp"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace omsi::map {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kWgs84R = 6378137.0;

double clampd(double v, double lo, double hi) { return (std::max)(lo, (std::min)(v, hi)); }

std::string body_to_string(const std::vector<std::uint8_t>& body) {
    return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

std::optional<std::string> json_string_field(std::string_view json, std::string_view key) {
    const std::string needle = std::string("\"") + std::string(key) + "\":\"";
    const auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    const auto start = pos + needle.size();
    const auto end = json.find('"', start);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(start, end - start));
}

std::vector<double> json_elevation_values(std::string_view json) {
    std::vector<double> out;
    std::size_t pos = 0;
    const std::string_view key = "\"elevation\"";
    while ((pos = json.find(key, pos)) != std::string_view::npos) {
        pos += key.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) ++pos;
        if (pos >= json.size()) break;
        if (json.compare(pos, 4, "null") == 0) {
            out.push_back(std::numeric_limits<double>::quiet_NaN());
            pos += 4;
            continue;
        }
        const auto end = json.find_first_of(",}", pos);
        if (end == std::string_view::npos) break;
        try {
            out.push_back(std::stod(std::string(json.substr(pos, end - pos))));
        } catch (...) {
            out.push_back(std::numeric_limits<double>::quiet_NaN());
        }
        pos = end;
    }
    return out;
}

// Open-Meteo: {"elevation":[38.0, 12.5]} — json_elevation_values expects a scalar after the key.
std::vector<double> json_elevation_array(std::string_view json) {
    std::vector<double> out;
    const auto key = json.find("\"elevation\"");
    if (key == std::string_view::npos) return out;
    const auto bra = json.find('[', key);
    if (bra == std::string_view::npos) return out;
    std::size_t pos = bra + 1;
    while (pos < json.size() && json[pos] != ']') {
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ','))
            ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json.compare(pos, 4, "null") == 0) {
            out.push_back(std::numeric_limits<double>::quiet_NaN());
            pos += 4;
            continue;
        }
        const auto end = json.find_first_of(",]", pos);
        if (end == std::string_view::npos) break;
        try {
            out.push_back(std::stod(std::string(json.substr(pos, end - pos))));
        } catch (...) {
            out.push_back(std::numeric_limits<double>::quiet_NaN());
        }
        pos = end;
    }
    return out;
}

void fill_nan_heights(std::vector<float>& heights, const int dim) {
    if (dim < 2 || static_cast<int>(heights.size()) != dim * dim) return;
    const auto at = [&](int x, int y) -> float& {
        return heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) + static_cast<std::size_t>(x)];
    };
    for (int pass = 0; pass < dim * 2; ++pass) {
        bool any_nan = false;
        for (int y = 0; y < dim; ++y) {
            for (int x = 0; x < dim; ++x) {
                float& h = at(x, y);
                if (!std::isnan(h)) continue;
                any_nan = true;
                float sum = 0.f;
                int count = 0;
                if (x > 0 && !std::isnan(at(x - 1, y))) {
                    sum += at(x - 1, y);
                    ++count;
                }
                if (x + 1 < dim && !std::isnan(at(x + 1, y))) {
                    sum += at(x + 1, y);
                    ++count;
                }
                if (y > 0 && !std::isnan(at(x, y - 1))) {
                    sum += at(x, y - 1);
                    ++count;
                }
                if (y + 1 < dim && !std::isnan(at(x, y + 1))) {
                    sum += at(x, y + 1);
                    ++count;
                }
                if (count > 0) h = sum / static_cast<float>(count);
            }
        }
        if (!any_nan) break;
    }
    for (auto& h : heights)
        if (std::isnan(h)) h = 0.f;
}

double mercator_pixel_x(const double lon, const int z) {
    const double n = std::pow(2.0, z);
    return (lon + 180.0) / 360.0 * n * 256.0;
}

double mercator_pixel_y(const double lat, const int z) {
    const double lat_rad = clampd(lat, -85.05112878, 85.05112878) * kPi / 180.0;
    const double n = std::pow(2.0, z);
    return (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) / 2.0 * n * 256.0;
}

std::string tile_xy_to_quadkey(const int x, const int y, const int z) {
    std::string q;
    q.reserve(static_cast<std::size_t>(z));
    for (int i = z; i > 0; --i) {
        char digit = '0';
        const int mask = 1 << (i - 1);
        if ((x & mask) != 0) digit++;
        if ((y & mask) != 0) digit += 2;
        q += digit;
    }
    return q;
}

bool crop_mosaic_to_bounds(const std::vector<std::uint8_t>& mosaic, const int mosaic_w, const int mosaic_h,
                           const int x0, const int y0, const int z, const TileWgs84Bounds& bounds,
                           std::vector<std::uint8_t>& cropped, int& crop_w, int& crop_h,
                           const int tile_px = 256) {
    // mercator_pixel_* is 256-based; multiply when Google mt scale=2/4 serves 512/1024 px tiles.
    const double s = static_cast<double>((std::max)(1, tile_px)) / 256.0;
    const double px_w = (mercator_pixel_x(bounds.west, z) - static_cast<double>(x0) * 256.0) * s;
    const double px_e = (mercator_pixel_x(bounds.east, z) - static_cast<double>(x0) * 256.0) * s;
    const double px_n = (mercator_pixel_y(bounds.north, z) - static_cast<double>(y0) * 256.0) * s;
    const double px_s = (mercator_pixel_y(bounds.south, z) - static_cast<double>(y0) * 256.0) * s;
    int left = static_cast<int>(std::floor((std::min)(px_w, px_e)));
    int right = static_cast<int>(std::ceil((std::max)(px_w, px_e)));
    int top = static_cast<int>(std::floor((std::min)(px_n, px_s)));
    int bottom = static_cast<int>(std::ceil((std::max)(px_n, px_s)));
    left = static_cast<int>(clampd(left, 0, mosaic_w - 1));
    right = static_cast<int>(clampd(right, left + 1, mosaic_w));
    top = static_cast<int>(clampd(top, 0, mosaic_h - 1));
    bottom = static_cast<int>(clampd(bottom, top + 1, mosaic_h));
    crop_w = right - left;
    crop_h = bottom - top;
    if (crop_w <= 0 || crop_h <= 0) return false;
    cropped.assign(static_cast<std::size_t>(crop_w) * static_cast<std::size_t>(crop_h) * 3, 0);
    for (int y = 0; y < crop_h; ++y) {
        const std::size_t src_off =
            (static_cast<std::size_t>(top + y) * static_cast<std::size_t>(mosaic_w) + static_cast<std::size_t>(left)) *
            3;
        const std::size_t dst_off = static_cast<std::size_t>(y) * static_cast<std::size_t>(crop_w) * 3;
        std::memcpy(cropped.data() + dst_off, mosaic.data() + src_off,
                    static_cast<std::size_t>(crop_w) * 3);
    }
    return true;
}

// Ground meters of WGS84 footprint (matches 611.5·cos(lat) for [worldcoordinates] tiles).
float tile_metric_size_from_bounds(const TileWgs84Bounds& b) {
    if (!(b.south < b.north && b.west < b.east)) return 0.f;
    const double lat_mid = (b.south + b.north) * 0.5;
    const double west_m = kWgs84R * (b.west * kPi / 180.0);
    const double east_m = kWgs84R * (b.east * kPi / 180.0);
    const double south_m = kWgs84R * std::log(std::tan(kPi * 0.25 + (b.south * kPi / 180.0) * 0.5));
    const double north_m = kWgs84R * std::log(std::tan(kPi * 0.25 + (b.north * kPi / 180.0) * 0.5));
    const double merc_w = std::abs(east_m - west_m);
    const double merc_h = std::abs(north_m - south_m);
    const double cos_lat = std::cos(lat_mid * kPi / 180.0);
    const double ground = ((std::max)(merc_w, merc_h)) * cos_lat;
    return static_cast<float>(ground);
}

double mercator_y_to_lat(double merc_y) {
    return std::atan(std::sinh(merc_y / kWgs84R)) * 180.0 / kPi;
}

double mercator_x_to_lon(double merc_x) { return merc_x * 180.0 / kPi / kWgs84R; }

double meters_per_degree_lat() { return 111132.92; }

double meters_per_degree_lon(double lat_deg) { return 111412.84 * std::cos(lat_deg * kPi / 180.0); }

void latlon_from_classic_grid(std::int32_t grid_x, std::int32_t grid_y, double ref_lat, double ref_lon,
                              double size_m, TileWgs84Bounds& b) {
    const double m_lat = meters_per_degree_lat();
    const double m_lon = meters_per_degree_lon(ref_lat);
    const double west_m = static_cast<double>(grid_x) * size_m;
    const double south_m = static_cast<double>(grid_y) * size_m;
    b.west = ref_lon + west_m / m_lon;
    b.south = ref_lat + south_m / m_lat;
    b.east = b.west + size_m / m_lon;
    b.north = b.south + size_m / m_lat;
}

void latlon_from_world_grid(std::int32_t grid_x, std::int32_t grid_y, TileWgs84Bounds& b) {
    // OMSI [worldcoordinates] == Web Mercator XYZ at z=16 (Editor_LinkAerial / GCMods):
    //   url_x = grid_x + 32768,  url_y = 32767 - grid_y.
    // Do not use grid×611.5 with R=6378137: true z16 size is 2πR/2^16 ≈ 611.496 m, and that
    // ~0.004 m/tile error accumulates to ~20–40 m imagery offset at |grid|≈10k (streets "desfazadas").
    constexpr int kAerialZ = 16;
    const double n = static_cast<double>(1 << kAerialZ);
    const double tx = static_cast<double>(static_cast<int>(grid_x) + 32768);
    const double ty = static_cast<double>(32767 - static_cast<int>(grid_y));
    b.west = tx / n * 360.0 - 180.0;
    b.east = (tx + 1.0) / n * 360.0 - 180.0;
    const auto lat_at_tms_y = [&](double tile_y) {
        return std::atan(std::sinh(kPi * (1.0 - 2.0 * tile_y / n))) * 180.0 / kPi;
    };
    b.north = lat_at_tms_y(ty);
    b.south = lat_at_tms_y(ty + 1.0);
}

bool write_bmp24(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& rgb) {
    if (width <= 0 || height <= 0) return false;
    const std::size_t row_bytes = static_cast<std::size_t>((width * 3 + 3) & ~3);
    const std::size_t need = row_bytes * static_cast<std::size_t>(height);
    if (rgb.size() < need) return false;

    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    const std::uint32_t file_size = static_cast<std::uint32_t>(54 + need);
    const std::uint32_t data_offset = 54;
    const std::uint32_t dib_size = 40;
    const std::int32_t h = height;
    const std::uint16_t planes = 1;
    const std::uint16_t bpp = 24;

    out.put('B');
    out.put('M');
    out.write(reinterpret_cast<const char*>(&file_size), 4);
    const std::uint32_t reserved = 0;
    out.write(reinterpret_cast<const char*>(&reserved), 4);
    out.write(reinterpret_cast<const char*>(&data_offset), 4);
    out.write(reinterpret_cast<const char*>(&dib_size), 4);
    out.write(reinterpret_cast<const char*>(&width), 4);
    out.write(reinterpret_cast<const char*>(&h), 4);
    const std::uint16_t one = 1;
    out.write(reinterpret_cast<const char*>(&one), 2);
    out.write(reinterpret_cast<const char*>(&planes), 2);
    out.write(reinterpret_cast<const char*>(&bpp), 2);
    const std::uint32_t compression = 0;
    out.write(reinterpret_cast<const char*>(&compression), 4);
    const std::uint32_t image_size = static_cast<std::uint32_t>(need);
    out.write(reinterpret_cast<const char*>(&image_size), 4);
    const std::int32_t ppm = 3780;
    out.write(reinterpret_cast<const char*>(&ppm), 4);
    out.write(reinterpret_cast<const char*>(&ppm), 4);
    const std::uint32_t colors = 0;
    out.write(reinterpret_cast<const char*>(&colors), 4);
    out.write(reinterpret_cast<const char*>(&colors), 4);

    for (int y = height - 1; y >= 0; --y) {
        const auto* row = rgb.data() + static_cast<std::size_t>(y) * row_bytes;
        out.write(reinterpret_cast<const char*>(row), static_cast<std::streamsize>(row_bytes));
    }
    return static_cast<bool>(out);
}

int pick_zoom_for_tile(float tile_size_m, double lat_mid, int output_px = 512) {
    const int px = (std::max)(64, output_px);
    const double meters_per_pixel_target = static_cast<double>(tile_size_m) / static_cast<double>(px);
    const double lat_rad = lat_mid * kPi / 180.0;
    const double initial = 156543.03392804097 * std::cos(lat_rad);
    int z = 0;
    for (; z < 22; ++z) {
        const double mpp = initial / std::pow(2.0, z);
        if (mpp <= meters_per_pixel_target) break;
    }
    return static_cast<int>(clampd(z, 0, 21));
}

struct TileXY {
    int x = 0;
    int y = 0;
};

TileXY latlon_to_tile(double lat, double lon, int z) {
    const double lat_rad = clampd(lat, -85.05112878, 85.05112878) * kPi / 180.0;
    const double n = std::pow(2.0, z);
    const double x = (lon + 180.0) / 360.0 * n;
    const double y = (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) / 2.0 * n;
    TileXY t{};
    t.x = static_cast<int>(std::floor(x));
    t.y = static_cast<int>(std::floor(y));
    return t;
}

bool wgs84_bounds_sane(const TileWgs84Bounds& b) {
    if (!(b.south < b.north && b.west < b.east)) return false;
    if (b.south < -90. || b.north > 90. || b.west < -180. || b.east > 180.) return false;
    if (b.north - b.south > 2.0 || b.east - b.west > 2.0) return false;
    return true;
}

bool aerial_tile_grid_ok(const int x0, const int x1, const int y0, const int y1, GeoJobResult& r,
                           const int tile_px = 256) {
    const int nx = x1 - x0 + 1;
    const int ny = y1 - y0 + 1;
    const int tp = (std::max)(1, tile_px);
    // 8K @ ~500 m ≈ 33×33 @ z21; allow headroom for larger tiles.
    if (nx <= 0 || ny <= 0 || nx > 48 || ny > 48) {
        r.message = "Rango tiles invalido (" + std::to_string(nx) + "x" + std::to_string(ny) +
                    ") — revisa bounds / resolution";
        return false;
    }
    if (static_cast<std::int64_t>(nx) * static_cast<std::int64_t>(ny) > 1600) {
        r.message = "Demasiados tiles (" + std::to_string(nx * ny) +
                    ") — baja Resolution o revisa bounds";
        return false;
    }
    const std::int64_t bytes = static_cast<std::int64_t>(nx) * ny * tp * tp * 3;
    if (bytes > 512LL * 1024 * 1024) {
        r.message = "Mosaic demasiado grande (" + std::to_string(bytes / (1024 * 1024)) + " MB)";
        return false;
    }
    return true;
}

bool decode_image_rgb(const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& rgb, UINT& w, UINT& h) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    IWICStream* stream = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->CreateStream(&stream))) {
        if (SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
                                                   static_cast<DWORD>(bytes.size())))) {
            IWICBitmapDecoder* dec = nullptr;
            if (SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
                IWICBitmapFrameDecode* frame = nullptr;
                if (SUCCEEDED(dec->GetFrame(0, &frame))) {
                    IWICFormatConverter* conv = nullptr;
                    if (SUCCEEDED(factory->CreateFormatConverter(&conv))) {
                        if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                                       nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
                            UINT fw = 0, fh = 0;
                            conv->GetSize(&fw, &fh);
                            rgb.resize(static_cast<std::size_t>(fw) * static_cast<std::size_t>(fh) * 3);
                            if (SUCCEEDED(conv->CopyPixels(nullptr, fw * 3, static_cast<UINT>(rgb.size()),
                                                            rgb.data()))) {
                                w = fw;
                                h = fh;
                                ok = true;
                            }
                        }
                        conv->Release();
                    }
                    frame->Release();
                }
                dec->Release();
            }
        }
        stream->Release();
    }
    factory->Release();
    return ok;
}

void blit_bgr(std::vector<std::uint8_t>& dst, int dst_w, int dst_h, int dx, int dy, const std::vector<std::uint8_t>& src,
              int src_w, int src_h) {
    for (int y = 0; y < src_h; ++y) {
        const int oy = dy + y;
        if (oy < 0 || oy >= dst_h) continue;
        for (int x = 0; x < src_w; ++x) {
            const int ox = dx + x;
            if (ox < 0 || ox >= dst_w) continue;
            const std::size_t di = (static_cast<std::size_t>(oy) * static_cast<std::size_t>(dst_w) +
                                    static_cast<std::size_t>(ox)) *
                                   3;
            const std::size_t si = (static_cast<std::size_t>(y) * static_cast<std::size_t>(src_w) +
                                    static_cast<std::size_t>(x)) *
                                   3;
            if (di + 2 < dst.size() && si + 2 < src.size()) {
                dst[di] = src[si];
                dst[di + 1] = src[si + 1];
                dst[di + 2] = src[si + 2];
            }
        }
    }
}

void resize_nearest_bgr(const std::vector<std::uint8_t>& src, int sw, int sh, std::vector<std::uint8_t>& dst, int dw,
                        int dh) {
    dst.assign(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 3, 0);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        const int sy = y * sh / dh;
        for (int x = 0; x < dw; ++x) {
            const int sx = x * sw / dw;
            const std::size_t di = (static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) +
                                    static_cast<std::size_t>(x)) *
                                   3;
            const std::size_t si = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) +
                                    static_cast<std::size_t>(sx)) *
                                   3;
            dst[di] = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }
}

void resize_bilinear_bgr(const std::vector<std::uint8_t>& src, int sw, int sh, std::vector<std::uint8_t>& dst, int dw,
                         int dh) {
    dst.assign(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 3, 0);
    if (sw <= 1 || sh <= 1 || dw <= 0 || dh <= 0) {
        resize_nearest_bgr(src, sw, sh, dst, dw, dh);
        return;
    }
    for (int y = 0; y < dh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const int y1 = (std::min)(y0 + 1, sh - 1);
        const float ty = fy - static_cast<float>(y0);
        const int yy0 = (std::max)(0, y0);
        for (int x = 0; x < dw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const int x1 = (std::min)(x0 + 1, sw - 1);
            const float tx = fx - static_cast<float>(x0);
            const int xx0 = (std::max)(0, x0);
            const std::size_t di = (static_cast<std::size_t>(y) * static_cast<std::size_t>(dw) +
                                    static_cast<std::size_t>(x)) *
                                   3;
            for (int c = 0; c < 3; ++c) {
                const auto sample = [&](int sx, int sy) -> float {
                    const std::size_t si =
                        (static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) + static_cast<std::size_t>(sx)) *
                        3;
                    return static_cast<float>(src[si + static_cast<std::size_t>(c)]);
                };
                const float v00 = sample(xx0, yy0);
                const float v10 = sample(x1, yy0);
                const float v01 = sample(xx0, y1);
                const float v11 = sample(x1, y1);
                const float v0 = v00 * (1.f - tx) + v10 * tx;
                const float v1 = v01 * (1.f - tx) + v11 * tx;
                dst[di + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(clampd(v0 * (1.f - ty) + v1 * ty, 0.0, 255.0));
            }
        }
    }
}

} // namespace

std::string expand_aerial_url_template(std::string tmpl, const int z, const int x, const int y,
                                       const std::string& api_key) {
    const auto replace_all = [&](const std::string& from, const std::string& to) {
        if (from.empty()) return;
        std::size_t pos = 0;
        while ((pos = tmpl.find(from, pos)) != std::string::npos) {
            tmpl.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("{z3857}", "EPSG:3857:" + std::to_string(z));
    replace_all("{z}", std::to_string(z));
    replace_all("{x}", std::to_string(x));
    const int tms_y = (1 << z) - 1 - y;
    replace_all("{ty}", std::to_string(tms_y));
    replace_all("{-y}", std::to_string(tms_y));
    replace_all("{y}", std::to_string(y));
    replace_all("{apikey}", api_key);
    replace_all("{key}", api_key);
    {
        std::string qk;
        qk.reserve(static_cast<std::size_t>(z));
        for (int i = z; i > 0; --i) {
            char digit = '0';
            const int mask = 1 << (i - 1);
            if ((x & mask) != 0) digit++;
            if ((y & mask) != 0) digit += 2;
            qk += digit;
        }
        replace_all("{quadkey}", qk);
    }
    return tmpl;
}

bool bounds_in_poland(const TileWgs84Bounds& bounds) {
    if (!(bounds.south < bounds.north && bounds.west < bounds.east)) return false;
    return bounds.south >= 49.0 && bounds.north <= 54.95 && bounds.west >= 14.0 && bounds.east <= 24.6;
}

bool bounds_in_caba(const TileWgs84Bounds& bounds) {
    if (!(bounds.south < bounds.north && bounds.west < bounds.east)) return false;
    return bounds.south >= -34.78 && bounds.north <= -34.50 && bounds.west >= -58.68 && bounds.east <= -58.28;
}

bool bounds_in_argentina(const TileWgs84Bounds& bounds) {
    if (!(bounds.south < bounds.north && bounds.west < bounds.east)) return false;
    return bounds.south >= -55.5 && bounds.north <= -21.0 && bounds.west >= -73.8 && bounds.east <= -53.0;
}

std::string format_tile_wgs84_summary(const TileWgs84Bounds& bounds) {
    std::ostringstream q;
    q << std::fixed << std::setprecision(5);
    q << "N " << bounds.north << " S " << bounds.south << " | W " << bounds.west << " E " << bounds.east;
    return q.str();
}

bool write_aerial_sidecar(const std::filesystem::path& jpg_path, const std::string& source,
                          const std::string& extra_meta) {
    const auto meta_path = std::filesystem::path(jpg_path.string() + ".meta.txt");
    omsi::fsutil::break_link(meta_path); // copy-on-write workspace: never write through a link
    std::ofstream out(meta_path, std::ios::trunc);
    if (!out) return false;
    out << "source=" << source << "\r\n";
    out << "crs=EPSG:3857 (Web Mercator)\r\n";
    if (!extra_meta.empty()) out << extra_meta;
    return true;
}

const char* custom_aerial_preset_label(const CustomAerialPreset p) {
    switch (p) {
    case CustomAerialPreset::User:
        return "Custom URL";
    case CustomAerialPreset::EsriWorldImagery:
        return "Esri World Imagery";
    case CustomAerialPreset::OpenStreetMap:
        return "OpenStreetMap";
    case CustomAerialPreset::EoxSentinel2:
        return "EOX Sentinel-2";
    case CustomAerialPreset::GeoportalPlOrto:
        return "Geoportal PL ORTO (standard)";
    case CustomAerialPreset::GeoportalPlOrtoHiRes:
        return "Geoportal PL ORTO (hi-res)";
    case CustomAerialPreset::BingAerialTemplate:
        return "Bing aerial ({apikey})";
    case CustomAerialPreset::GcbaAerial2017:
        return "GCBA CABA orthophoto 2017 (TMS {ty})";
    case CustomAerialPreset::IgnArgentinaVant:
        return "IGN Argentina VANT mosaic (TMS {ty})";
    }
    return "Custom";
}

const char* custom_aerial_preset_url(const CustomAerialPreset p) {
    switch (p) {
    case CustomAerialPreset::EsriWorldImagery:
        return "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
    case CustomAerialPreset::OpenStreetMap:
        return "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    case CustomAerialPreset::EoxSentinel2:
        return "https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless-2021_3857/default/GoogleMapsCompatible/{z}/{y}/{x}.jpg";
    case CustomAerialPreset::GeoportalPlOrto:
        return "https://mapy.geoportal.gov.pl/wss/service/PZGIK/ORTO/WMTS/StandardResolution?"
               "SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=ORTOFOTOMAPA&STYLE=default&FORMAT=image/jpeg&"
               "tileMatrixSet=EPSG:3857&tileMatrix={z3857}&tileRow={y}&tileCol={x}";
    case CustomAerialPreset::GeoportalPlOrtoHiRes:
        return "https://mapy.geoportal.gov.pl/wss/service/PZGIK/ORTO/WMTS/HighResolution?"
               "SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=ORTOFOTOMAPA&STYLE=default&FORMAT=image/jpeg&"
               "tileMatrixSet=EPSG:3857&tileMatrix={z3857}&tileRow={y}&tileCol={x}";
    case CustomAerialPreset::BingAerialTemplate:
        return "https://t0.tiles.virtualearth.net/tiles/a{quadkey}.jpeg?key={apikey}";
    case CustomAerialPreset::GcbaAerial2017:
        return "http://servicios.usig.buenosaires.gob.ar/mapcache/tms/1.0.0/"
               "fotografias_aereas_2017_caba_3857@GoogleMapsCompatible/{z}/{x}/{ty}.png";
    case CustomAerialPreset::IgnArgentinaVant:
        return "https://imagenes.ign.gob.ar/geoserver/gwc/service/tms/1.0.0/"
               "mosaicos_vuelos_vant@EPSG%3A3857@png/{z}/{x}/{ty}.png";
    default:
        return "";
    }
}

MapCoordKind detect_map_coord_kind(const GlobalMap& global) {
    if (global.world_coordinates) return MapCoordKind::WorldCoordinates;
    for (const auto& ref : global.tiles) {
        const auto stem = ref.tile_path.stem().string();
        if (stem.rfind("tile_", 0) != 0 && stem.size() >= 5) {
            bool all_digit = true;
            for (char c : stem)
                if (!std::isdigit(static_cast<unsigned char>(c))) all_digit = false;
            if (all_digit) return MapCoordKind::GlobalTileNames;
        }
    }
    return MapCoordKind::Classic;
}

std::string map_coord_kind_label(const MapCoordKind kind) {
    switch (kind) {
    case MapCoordKind::Classic:
        return "Classic (300 m tiles)";
    case MapCoordKind::WorldCoordinates:
        return "World coordinates (Web Mercator EPSG:3857)";
    case MapCoordKind::GlobalTileNames:
        return "Global tile names (lat-coded)";
    }
    return "Unknown";
}

bool enable_world_coordinates(const std::filesystem::path& global_cfg) {
    if (global_cfg.empty() || !std::filesystem::exists(global_cfg)) return false;
    const auto doc = cfg::load_cfg_file(global_cfg);
    if (doc && doc->contains("[worldcoordinates]")) return false;

    omsi::fsutil::break_link(global_cfg); // copy-on-write workspace: never write through a link
    std::ofstream out(global_cfg, std::ios::app);
    if (!out) return false;
    out << "\r\n[worldcoordinates]\r\n\r\n";
    log::info("enabled [worldcoordinates] in " + global_cfg.string());
    return true;
}

OmsiAerialUrlCoords omsi_aerial_url_coords(const std::int32_t grid_x, const std::int32_t grid_y) {
    OmsiAerialUrlCoords c{};
    c.url_x = static_cast<int>(grid_x) + 32768;
    c.url_y = 32767 - static_cast<int>(grid_y);
    return c;
}

int aerial_resolution_label_px(const int trackbar_pos) {
    const int pos = (std::max)(0, (std::min)(5, trackbar_pos)); // 256..8192
    return 1 << (pos + 8);
}

int effective_aerial_download_px(const SatelliteProvider provider, const int resolution_pos,
                                 const std::string& link_aerial) {
    const int res_pos = (std::max)(0, (std::min)(5, resolution_pos));
    if (provider == SatelliteProvider::OmsiLinkAerial || provider == SatelliteProvider::GcmodsBing ||
        provider == SatelliteProvider::GcmodsGoogle) {
        std::string link = link_aerial;
        if (provider == SatelliteProvider::GcmodsGoogle) link = gcmods_google_satellite_link();
        const bool google_hres = provider == SatelliteProvider::GcmodsGoogle ||
                                 link.find("service=google") != std::string::npos;
        const int use_pos = google_hres ? 0 : res_pos;
        return (1 << use_pos) * 256;
    }
    return aerial_resolution_label_px(res_pos);
}

std::optional<std::pair<int, int>> read_jpeg_dimensions(const std::filesystem::path& jpg_path) {
    if (jpg_path.empty() || !std::filesystem::exists(jpg_path)) return std::nullopt;
    std::ifstream in(jpg_path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return std::nullopt;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return std::nullopt;

    std::optional<std::pair<int, int>> out;
    IWICStream* stream = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
                                               static_cast<DWORD>(bytes.size())))) {
        IWICBitmapDecoder* dec = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(dec->GetFrame(0, &frame))) {
                UINT w = 0, h = 0;
                if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0)
                    out = std::make_pair(static_cast<int>(w), static_cast<int>(h));
                frame->Release();
            }
            dec->Release();
        }
    }
    if (stream) stream->Release();
    factory->Release();
    return out;
}

void remove_tile_aerial_files(const std::filesystem::path& omsi_root, const std::filesystem::path& map_root,
                              const std::filesystem::path& tile_path) {
    const auto filename = tile_path.stem().string() + ".aerial.jpg";
    std::vector<std::filesystem::path> candidates = {
        tile_aerial_jpg_path(omsi_root, map_root, tile_path),
        map_root / "aerials" / filename,
        tile_path.parent_path() / "aerials" / filename,
    };
    if (!omsi_root.empty()) {
        std::string subdir;
        const auto parent = tile_path.parent_path().generic_string();
        if (!parent.empty()) subdir = parent.substr(0, (std::min)(parent.size(), std::size_t{5}));
        if (subdir.empty()) subdir = "local";
        candidates.push_back(omsi_root / "maps" / "aerials" / subdir / filename);
    }
    for (const auto& p : candidates) {
        if (p.empty() || !std::filesystem::exists(p)) continue;
        std::error_code ec;
        std::filesystem::remove(p, ec);
        const auto meta = std::filesystem::path(p.string() + ".meta.txt");
        if (std::filesystem::exists(meta)) std::filesystem::remove(meta, ec);
    }
}

const char* satellite_provider_label(const SatelliteProvider p) {
    switch (p) {
    case SatelliteProvider::EsriWorldImagery:
        return "Esri World Imagery (free)";
    case SatelliteProvider::EoxSentinel2:
        return "EOX Sentinel-2 (free)";
    case SatelliteProvider::OmsiLinkAerial:
        return "OMSI Editor_LinkAerial";
    case SatelliteProvider::MapTiler:
        return "MapTiler satellite (free key)";
    case SatelliteProvider::GoogleMapTiles:
        return "Google Map Tiles (paid)";
    case SatelliteProvider::OpenStreetMap:
        return "OpenStreetMap (street map, free)";
    case SatelliteProvider::BingAerial:
        return "Bing Maps aerial (free Azure key)";
    case SatelliteProvider::GoogleStaticMaps:
        return "Google Static Maps satellite (API key)";
    case SatelliteProvider::GeoportalPlOrto:
        return "Geoportal PL orthophoto (Poland)";
    case SatelliteProvider::GeoportalPlOrtoHiRes:
        return "Geoportal PL orthophoto hi-res (Poland)";
    case SatelliteProvider::CustomAerialUrl:
        return "Custom URL template (presets)";
    case SatelliteProvider::GcbaAerial2017:
        return "GCBA CABA orthophoto 2017 (free, TMS)";
    case SatelliteProvider::IgnArgentinaVant:
        return "IGN Argentina VANT orthophoto (free, TMS)";
    case SatelliteProvider::GcmodsBing:
        return "GCMods Bing satellite (OMSI Editor_LinkAerial)";
    case SatelliteProvider::GcmodsGoogle:
        return "GCMods Google satellite (OMSI, 256px)";
    case SatelliteProvider::GoogleMtSatellite:
        return "Google mt* satellite scale=2/4 (experimental, no key)";
    }
    return "Unknown";
}

const char* elevation_provider_label(const ElevationProvider p) {
    switch (p) {
    case ElevationProvider::OpenTopoData:
        return "OpenTopoData ASTER 30m (free)";
    case ElevationProvider::Google:
        return "Google Elevation (paid)";
    case ElevationProvider::NasaSrtm3:
        return "NASA SRTM 30m (OpenTopoData, free)";
    case ElevationProvider::GeoportalPlNmt:
        return "Geoportal PL NMT 1m (Poland only)";
    case ElevationProvider::Terrarium:
        return "Terrarium XYZ (tilezen/joerd, free)";
    case ElevationProvider::OpenMeteo:
        return "Open-Meteo Copernicus GLO-90 (free)";
    case ElevationProvider::OpenElevation:
        return "Open-Elevation (github.com/Jorl17, free)";
    case ElevationProvider::Mapterhorn:
        return "Mapterhorn Terrarium WebP (keyless)";
    case ElevationProvider::CopernicusGlo30:
        return "Copernicus GLO-30 COG (AWS, ~30m DSM)";
    case ElevationProvider::Fabdem:
        return "FABDEM V1-2 (DTM, buildings/trees removed)";
    }
    return "Unknown";
}

std::wstring satellite_tile_url(const SatelliteProvider provider, const int z, const int x, const int y,
                                const std::string& api_key, const std::string& custom_url_template,
                                const int google_mt_scale = 2) {
    std::ostringstream q;
    switch (provider) {
    case SatelliteProvider::EsriWorldImagery:
        q << "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/" << z << '/' << y
          << '/' << x;
        break;
    case SatelliteProvider::EoxSentinel2:
        q << "https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless-2021_3857/default/GoogleMapsCompatible/" << z << '/'
          << y << '/' << x << ".jpg";
        break;
    case SatelliteProvider::MapTiler:
        q << "https://api.maptiler.com/tiles/satellite-v2/" << z << '/' << x << '/' << y << ".jpg?key=" << api_key;
        break;
    case SatelliteProvider::OpenStreetMap:
        q << "https://tile.openstreetmap.org/" << z << '/' << x << '/' << y << ".png";
        break;
    case SatelliteProvider::BingAerial:
        if (api_key.empty()) return L"";
        q << "https://t" << ((x + y) % 4) << ".tiles.virtualearth.net/tiles/a"
          << tile_xy_to_quadkey(x, y, z) << ".jpeg?key=" << api_key;
        break;
    case SatelliteProvider::GeoportalPlOrto:
        q << "https://mapy.geoportal.gov.pl/wss/service/PZGIK/ORTO/WMTS/StandardResolution?"
          << "SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=ORTOFOTOMAPA&STYLE=default&FORMAT=image/jpeg&"
          << "tileMatrixSet=EPSG:3857&tileMatrix=EPSG:3857:" << z << "&tileRow=" << y << "&tileCol=" << x;
        break;
    case SatelliteProvider::GeoportalPlOrtoHiRes:
        q << "https://mapy.geoportal.gov.pl/wss/service/PZGIK/ORTO/WMTS/HighResolution?"
          << "SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=ORTOFOTOMAPA&STYLE=default&FORMAT=image/jpeg&"
          << "tileMatrixSet=EPSG:3857&tileMatrix=EPSG:3857:" << z << "&tileRow=" << y << "&tileCol=" << x;
        break;
    case SatelliteProvider::CustomAerialUrl:
        if (custom_url_template.empty()) return L"";
        q << expand_aerial_url_template(custom_url_template, z, x, y, api_key);
        break;
    case SatelliteProvider::GcbaAerial2017: {
        const int tms_y = (1 << z) - 1 - y;
        q << "http://servicios.usig.buenosaires.gob.ar/mapcache/tms/1.0.0/"
             "fotografias_aereas_2017_caba_3857@GoogleMapsCompatible/" << z << '/' << x << '/' << tms_y << ".png";
        break;
    }
    case SatelliteProvider::IgnArgentinaVant: {
        const int tms_y = (1 << z) - 1 - y;
        q << "https://imagenes.ign.gob.ar/geoserver/gwc/service/tms/1.0.0/"
             "mosaicos_vuelos_vant@EPSG%3A3857@png/" << z << '/' << x << '/' << tms_y << ".png";
        break;
    }
    case SatelliteProvider::GoogleMtSatellite: {
        // Unofficial Google Maps satellite XYZ. scale=2→512px, scale=4→1024px (same geo footprint).
        const int sc = (google_mt_scale == 4) ? 4 : ((google_mt_scale == 1) ? 1 : 2);
        q << "https://mt" << ((x + y) % 4) << ".google.com/vt/lyrs=s&x=" << x << "&y=" << y << "&z=" << z
          << "&scale=" << sc;
        break;
    }
    case SatelliteProvider::GoogleMapTiles:
    case SatelliteProvider::GoogleStaticMaps:
        return L"";
    default:
        return L"";
    }
    const std::string s = q.str();
    return omsi::utf8_to_wide(s);
}

std::string build_editor_aerial_url(const std::string& link_template, const int url_x, const int url_y,
                                    const int url_z, const std::string& api_key) {
    // OMSI options.cfg [Editor_LinkAerial]: replace ~x/~y/~z in-place (GCMods / OSM / etc.).
    std::string out = link_template;
    const auto replace_all = [&](const std::string& from, const std::string& to) {
        if (from.empty()) return;
        std::size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all("~x", std::to_string(url_x));
    replace_all("~y", std::to_string(url_y));
    replace_all("~z", std::to_string(url_z));
    replace_all("[API-Code]", api_key);
    replace_all("[Key]", api_key);
    replace_all("{apikey}", api_key);
    replace_all("{key}", api_key);
    return out;
}

const char* gcmods_bing_satellite_link() {
    // https://www.gcmods.de/luftbilder/omsi/editor/2017/10/01/luftbilder-in-omsi-2
    return "http://aerial.gcmods.de/toquad.php?x=~x&y=~y&z=~z&apicode=[API-Code]";
}

const char* gcmods_google_satellite_link() {
    // Google via GCMods: only 256 px (hres=1) works per GCMods docs.
    return "http://aerial.gcmods.de/toquad.php?x=~x&y=~y&z=~z&service=google&hres=1&apicode=[API-Code]";
}

const char* gcmods_osm_street_link() {
    return "http://b.tile.openstreetmap.org/~z/~x/~y.png";
}

std::filesystem::path map_aerials_dir(const std::filesystem::path& omsi_root,
                                      const std::filesystem::path& map_root) {
    const auto map_name = map_root.filename().string();
    if (!omsi_root.empty() && !map_name.empty())
        return omsi_root / "maps" / "aerials" / map_name;
    return map_root / "aerials";
}

std::filesystem::path tile_aerial_jpg_path(const std::filesystem::path& omsi_root,
                                           const std::filesystem::path& map_root,
                                           const std::filesystem::path& tile_path) {
    return map_aerials_dir(omsi_root, map_root) / (tile_path.stem().string() + ".aerial.jpg");
}

std::optional<std::filesystem::path> find_tile_aerial_jpg(const std::filesystem::path& omsi_root,
                                                            const std::filesystem::path& map_root,
                                                            const std::filesystem::path& tile_path) {
    const auto filename = tile_path.stem().string() + ".aerial.jpg";
    std::vector<std::filesystem::path> candidates = {
        tile_aerial_jpg_path(omsi_root, map_root, tile_path),
        map_root / "aerials" / filename,
        tile_path.parent_path() / "aerials" / filename,
    };
    if (!omsi_root.empty()) {
        std::string subdir;
        const auto parent = tile_path.parent_path().generic_string();
        if (!parent.empty()) subdir = parent.substr(0, (std::min)(parent.size(), std::size_t{5}));
        if (subdir.empty()) subdir = "local";
        candidates.push_back(omsi_root / "maps" / "aerials" / subdir / filename);
    }
    for (const auto& p : candidates) {
        if (!p.empty() && std::filesystem::exists(p)) return p;
    }
    return std::nullopt;
}

bool write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool write_jpeg_rgb(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& bgr) {
    if (width <= 0 || height <= 0) return false;
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    IWICStream* stream = nullptr;
    IWICBitmap* bitmap = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;

    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) {
        if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
            if (SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props))) {
                frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
                WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                frame->SetPixelFormat(&fmt);
                if (SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width * 3),
                                                 static_cast<UINT>(bgr.size()), const_cast<BYTE*>(bgr.data())))) {
                    ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
                }
            }
        }
    }

    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (bitmap) bitmap->Release();
    if (stream) stream->Release();
    factory->Release();
    return ok;
}

GeoJobResult download_tile_aerial_omsi(const std::string& link_template, const std::int32_t grid_x,
                                         const std::int32_t grid_y, const int resolution_pos,
                                         const std::filesystem::path& out_jpg, const std::string& api_key) {
    GeoJobResult r{};
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoScope {
        bool active = false;
        ~CoScope() {
            if (active) CoUninitialize();
        }
    } co_scope;
    co_scope.active = SUCCEEDED(co_hr);

    if (link_template.empty()) {
        r.message = "Editor_LinkAerial vacio (opciones OMSI / GCMods)";
        return r;
    }
    if (link_template.find("[API-Code]") != std::string::npos && api_key.empty()) {
        r.message = "GCMods: ingresa API key (Bing Azure o Google) en el campo API key";
        return r;
    }

    // OMSİ world tile at z=16; higher resolution subdivides (512→z17 2x2 … 8192→z21 32x32).
    const int res_pos = (std::max)(0, (std::min)(5, resolution_pos));
    const bool google_hres = link_template.find("service=google") != std::string::npos;
    const int use_pos = google_hres ? 0 : res_pos; // GCMods: Google solo 256 px
    const int grid_div = 1 << use_pos;
    const int url_z = 16 + use_pos;
    const int out_px = grid_div * 256;
    const auto base = omsi_aerial_url_coords(grid_x, grid_y);
    std::vector<std::uint8_t> mosaic(static_cast<std::size_t>(out_px) * static_cast<std::size_t>(out_px) * 3, 0);

    struct OmsiTileJob {
        int col = 0;
        int row = 0;
        std::vector<std::uint8_t> rgb;
        UINT w = 0;
        UINT h = 0;
        bool ok = false;
        int status = 0;
    };
    std::vector<OmsiTileJob> jobs;
    jobs.reserve(static_cast<std::size_t>(grid_div) * static_cast<std::size_t>(grid_div));
    for (int row = 0; row < grid_div; ++row)
        for (int col = 0; col < grid_div; ++col) jobs.push_back(OmsiTileJob{col, row, {}, 0, 0, false, 0});

    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n_workers =
        (std::min)(32u, (std::max)(4u, hw == 0 ? 8u : hw * 2u));
    std::atomic<std::size_t> next_job{0};
    std::atomic<int> got{0};
    std::atomic<int> last_status{0};
    std::vector<std::thread> pool;
    pool.reserve(n_workers);
    for (unsigned wi = 0; wi < n_workers; ++wi) {
        pool.emplace_back([&, link_template, api_key, base, grid_div, url_z]() {
            const HRESULT whr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool co_own = SUCCEEDED(whr);
            for (;;) {
                const std::size_t i = next_job.fetch_add(1, std::memory_order_relaxed);
                if (i >= jobs.size()) break;
                auto& job = jobs[i];
                const int sub_x = base.url_x * grid_div + job.col;
                const int sub_y = base.url_y * grid_div + job.row;
                const auto url_str = build_editor_aerial_url(link_template, sub_x, sub_y, url_z, api_key);
                const std::wstring url(url_str.begin(), url_str.end());
                const auto resp = http::http_get(url);
                job.status = resp.status;
                last_status.store(resp.status, std::memory_order_relaxed);
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
        blit_bgr(mosaic, out_px, out_px, job.col * 256, job.row * 256, job.rgb, static_cast<int>(job.w),
                 static_cast<int>(job.h));
    }

    if (got.load() == 0) {
        r.message = "Descarga fallida HTTP " + std::to_string(last_status.load()) +
                    " — revisa URL GCMods / API key / [worldcoordinates]";
        return r;
    }

    std::filesystem::create_directories(out_jpg.parent_path());
    if (!write_jpeg_rgb(out_jpg, out_px, out_px, mosaic)) {
        r.message = "Error escribiendo .aerial.jpg";
        return r;
    }

    r.ok = true;
    r.output_path = out_jpg;
    r.message = "GCMods/OMSI aerial OK " + std::to_string(out_px) + "px z=" + std::to_string(url_z) +
                " tiles=" + std::to_string(got.load()) + " workers=" + std::to_string(n_workers);
    if (google_hres && res_pos > 0)
        r.message += " (Google GCMods forzado a 256px)";
    return r;
}

#include "map/map_geo_tools_part2.inc.cpp"
} // namespace omsi::map

