#include "omsi/fs_cow.hpp"
#include "omsi/dem_geotiff.hpp"

#include "omsi/http_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../third_party/miniz/miniz.h"

namespace omsi::map {
namespace {

constexpr std::uint16_t kTagImageWidth = 256;
constexpr std::uint16_t kTagImageLength = 257;
constexpr std::uint16_t kTagBitsPerSample = 258;
constexpr std::uint16_t kTagCompression = 259;
constexpr std::uint16_t kTagSamplesPerPixel = 277;
constexpr std::uint16_t kTagPredictor = 317;
constexpr std::uint16_t kTagTileWidth = 322;
constexpr std::uint16_t kTagTileLength = 323;
constexpr std::uint16_t kTagTileOffsets = 324;
constexpr std::uint16_t kTagTileByteCounts = 325;
constexpr std::uint16_t kTagSampleFormat = 339;
constexpr std::uint16_t kTagModelPixelScale = 33550;
constexpr std::uint16_t kTagModelTiepoint = 33922;

struct TiffTag {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::uint32_t value_or_offset = 0;
};

struct CogDem {
    bool little_endian = true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t tile_w = 0;
    std::uint32_t tile_h = 0;
    std::uint16_t compression = 1;
    std::uint16_t predictor = 1;
    std::uint16_t sample_format = 1;
    std::uint16_t bits_per_sample = 32;
    double origin_lon = 0.;
    double origin_lat = 0.;
    double px_x = 1. / 3600.;
    double px_y = 1. / 3600.;
    std::vector<std::uint64_t> tile_offsets;
    std::vector<std::uint64_t> tile_byte_counts;
    std::vector<std::uint8_t> local;
    std::wstring http_url;
    std::string err;
};

bool wgs84_ok(const TileWgs84Bounds& b) {
    if (!(b.south < b.north && b.west < b.east)) return false;
    if (b.south < -90. || b.north > 90. || b.west < -180. || b.east > 180.) return false;
    if (b.north - b.south > 2.0 || b.east - b.west > 2.0) return false;
    return true;
}

void fill_nans(std::vector<float>& heights, const int dim) {
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

std::filesystem::path dem_cache_root() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::temp_directory_path() / "omsi_f4_dem_cache";
    return std::filesystem::path(base) / "omsi_f4" / "dem_cache";
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool starts_with_http(const std::string& s) {
    return s.size() >= 8 && (s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0);
}

bool read_local_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    out.resize(n);
    if (n && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n))) return false;
    return true;
}

bool write_local_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    if (!data.empty())
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool http_range(const std::wstring& url, std::uint64_t first, std::uint64_t last,
                std::vector<std::uint8_t>& out, std::string& err) {
    std::wostringstream hdr;
    hdr << L"Range: bytes=" << first << L'-' << last << L"\r\n";
    const auto resp = http::http_get(url, hdr.str());
    if (resp.status != 200 && resp.status != 206) {
        err = "HTTP " + std::to_string(resp.status) + (resp.error.empty() ? "" : (" " + resp.error));
        return false;
    }
    if (resp.body.empty()) {
        err = "empty range body";
        return false;
    }
    out = resp.body;
    return true;
}

std::uint16_t rd16(const std::uint8_t* p, bool le) {
    return le ? static_cast<std::uint16_t>(p[0] | (p[1] << 8))
              : static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t rd32(const std::uint8_t* p, bool le) {
    return le ? (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                 (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24))
              : ((static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                 (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]));
}
std::uint64_t rd64(const std::uint8_t* p, bool le) {
    const std::uint32_t a = rd32(p, le);
    const std::uint32_t b = rd32(p + 4, le);
    return le ? (static_cast<std::uint64_t>(a) | (static_cast<std::uint64_t>(b) << 32))
              : ((static_cast<std::uint64_t>(a) << 32) | b);
}
double rd_double(const std::uint8_t* p, bool le) {
    const std::uint64_t u = rd64(p, le);
    double d = 0.;
    std::memcpy(&d, &u, 8);
    return d;
}

bool ensure_bytes(CogDem& dem, std::uint64_t offset, std::uint64_t length, std::vector<std::uint8_t>& chunk) {
    if (!dem.local.empty()) {
        if (offset + length > dem.local.size()) {
            dem.err = "local TIFF truncated";
            return false;
        }
        chunk.assign(dem.local.begin() + static_cast<std::ptrdiff_t>(offset),
                     dem.local.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return true;
    }
    if (dem.http_url.empty()) {
        dem.err = "no TIFF source";
        return false;
    }
    return http_range(dem.http_url, offset, offset + length - 1, chunk, dem.err);
}

bool parse_ifd0(CogDem& dem, const std::vector<std::uint8_t>& head) {
    if (head.size() < 8) {
        dem.err = "TIFF header too small";
        return false;
    }
    if (head[0] == 'I' && head[1] == 'I') dem.little_endian = true;
    else if (head[0] == 'M' && head[1] == 'M') dem.little_endian = false;
    else {
        dem.err = "not a TIFF";
        return false;
    }
    if (rd16(head.data() + 2, dem.little_endian) != 42) {
        dem.err = "Classic TIFF only (no BigTIFF)";
        return false;
    }
    const std::uint32_t ifd0 = rd32(head.data() + 4, dem.little_endian);
    if (ifd0 + 2 > head.size()) {
        dem.err = "IFD0 outside header window";
        return false;
    }
    const std::uint16_t nent = rd16(head.data() + ifd0, dem.little_endian);
    if (ifd0 + 2u + static_cast<std::uint32_t>(nent) * 12u > head.size()) {
        dem.err = "IFD0 truncated in header window";
        return false;
    }
    std::vector<TiffTag> tags;
    tags.reserve(nent);
    for (std::uint16_t i = 0; i < nent; ++i) {
        const std::uint8_t* e = head.data() + ifd0 + 2 + i * 12;
        TiffTag t;
        t.tag = rd16(e, dem.little_endian);
        t.type = rd16(e + 2, dem.little_endian);
        t.count = rd32(e + 4, dem.little_endian);
        t.value_or_offset = rd32(e + 8, dem.little_endian);
        tags.push_back(t);
    }

    auto find = [&](std::uint16_t id) -> const TiffTag* {
        for (const auto& t : tags)
            if (t.tag == id) return &t;
        return nullptr;
    };
    auto scalar_u32 = [&](std::uint16_t id, std::uint32_t def) -> std::uint32_t {
        const auto* t = find(id);
        if (!t || t->count != 1) return def;
        if (t->type == 3) return dem.little_endian ? (t->value_or_offset & 0xFFFFu) : (t->value_or_offset >> 16);
        return t->value_or_offset;
    };

    dem.width = scalar_u32(kTagImageWidth, 0);
    dem.height = scalar_u32(kTagImageLength, 0);
    dem.tile_w = scalar_u32(kTagTileWidth, 0);
    dem.tile_h = scalar_u32(kTagTileLength, 0);
    dem.compression = static_cast<std::uint16_t>(scalar_u32(kTagCompression, 1));
    dem.predictor = static_cast<std::uint16_t>(scalar_u32(kTagPredictor, 1));
    dem.sample_format = static_cast<std::uint16_t>(scalar_u32(kTagSampleFormat, 1));
    dem.bits_per_sample = static_cast<std::uint16_t>(scalar_u32(kTagBitsPerSample, 32));
    const auto spp = scalar_u32(kTagSamplesPerPixel, 1);
    if (dem.width == 0 || dem.height == 0 || dem.tile_w == 0 || dem.tile_h == 0) {
        dem.err = "missing tiled geometry";
        return false;
    }
    if (dem.compression != 8 && dem.compression != 32946) {
        dem.err = "only Deflate GeoTIFF supported";
        return false;
    }
    if (dem.bits_per_sample != 32 || dem.sample_format != 3 || spp != 1) {
        dem.err = "only Float32 single-band DEM supported";
        return false;
    }

    const std::uint32_t tiles_x = (dem.width + dem.tile_w - 1) / dem.tile_w;
    const std::uint32_t tiles_y = (dem.height + dem.tile_h - 1) / dem.tile_h;
    const std::uint32_t ntiles = tiles_x * tiles_y;

    auto load_u32_array = [&](const TiffTag& t, std::vector<std::uint64_t>& out) -> bool {
        out.assign(t.count, 0);
        const std::size_t type_sz = (t.type == 3) ? 2u : (t.type == 4) ? 4u : (t.type == 16) ? 8u : 0u;
        if (type_sz == 0) {
            dem.err = "unsupported offset array type";
            return false;
        }
        const std::uint64_t nbytes = static_cast<std::uint64_t>(t.count) * type_sz;
        std::vector<std::uint8_t> raw;
        if (nbytes <= 4) {
            raw.resize(4);
            const std::uint32_t v = t.value_or_offset;
            if (dem.little_endian) {
                raw[0] = static_cast<std::uint8_t>(v);
                raw[1] = static_cast<std::uint8_t>(v >> 8);
                raw[2] = static_cast<std::uint8_t>(v >> 16);
                raw[3] = static_cast<std::uint8_t>(v >> 24);
            } else {
                raw[0] = static_cast<std::uint8_t>(v >> 24);
                raw[1] = static_cast<std::uint8_t>(v >> 16);
                raw[2] = static_cast<std::uint8_t>(v >> 8);
                raw[3] = static_cast<std::uint8_t>(v);
            }
        } else if (!ensure_bytes(dem, t.value_or_offset, nbytes, raw)) {
            return false;
        }
        for (std::uint32_t i = 0; i < t.count; ++i) {
            if (t.type == 3) out[i] = rd16(raw.data() + i * 2, dem.little_endian);
            else if (t.type == 4) out[i] = rd32(raw.data() + i * 4, dem.little_endian);
            else out[i] = rd64(raw.data() + i * 8, dem.little_endian);
        }
        return true;
    };

    const auto* toff = find(kTagTileOffsets);
    const auto* tbc = find(kTagTileByteCounts);
    if (!toff || !tbc || toff->count != ntiles || tbc->count != ntiles) {
        dem.err = "TileOffsets/TileByteCounts mismatch";
        return false;
    }
    if (!load_u32_array(*toff, dem.tile_offsets) || !load_u32_array(*tbc, dem.tile_byte_counts)) return false;

    dem.px_x = 1. / 3600.;
    dem.px_y = 1. / 3600.;
    dem.origin_lon = 0.;
    dem.origin_lat = 0.;
    if (const auto* ps = find(kTagModelPixelScale); ps && ps->type == 12 && ps->count >= 2) {
        std::vector<std::uint8_t> raw;
        if (!ensure_bytes(dem, ps->value_or_offset, ps->count * 8ull, raw)) return false;
        dem.px_x = rd_double(raw.data(), dem.little_endian);
        dem.px_y = std::abs(rd_double(raw.data() + 8, dem.little_endian));
    }
    if (const auto* tp = find(kTagModelTiepoint); tp && tp->type == 12 && tp->count >= 6) {
        std::vector<std::uint8_t> raw;
        if (!ensure_bytes(dem, tp->value_or_offset, tp->count * 8ull, raw)) return false;
        dem.origin_lon = rd_double(raw.data() + 3 * 8, dem.little_endian);
        dem.origin_lat = rd_double(raw.data() + 4 * 8, dem.little_endian);
    }
    if (dem.px_x <= 0. || dem.px_y <= 0.) {
        dem.err = "invalid pixel scale";
        return false;
    }
    return true;
}

bool open_dem(CogDem& dem, const std::string& url_or_path) {
    dem = CogDem{};
    if (starts_with_http(url_or_path)) {
        dem.http_url = utf8_to_wide(url_or_path);
        std::vector<std::uint8_t> head;
        if (!http_range(dem.http_url, 0, 512 * 1024 - 1, head, dem.err)) return false;
        return parse_ifd0(dem, head);
    }
    if (!read_local_file(url_or_path, dem.local)) {
        dem.err = "cannot open local TIFF";
        return false;
    }
    return parse_ifd0(dem, dem.local);
}

bool inflate_zlib(const std::uint8_t* src, std::size_t src_n, std::vector<std::uint8_t>& dst,
                  std::size_t expect) {
    dst.resize(expect);
    mz_ulong dest_len = static_cast<mz_ulong>(expect);
    if (mz_uncompress(dst.data(), &dest_len, src, static_cast<mz_ulong>(src_n)) == MZ_OK &&
        dest_len == expect) {
        dst.resize(dest_len);
        return true;
    }
    dest_len = static_cast<mz_ulong>(expect);
    tinfl_decompressor decomp;
    tinfl_init(&decomp);
    size_t in_sz = src_n;
    size_t out_sz = expect;
    const tinfl_status status =
        tinfl_decompress(&decomp, src, &in_sz, dst.data(), dst.data(), &out_sz,
                         TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || out_sz != expect) return false;
    dst.resize(out_sz);
    return true;
}

void apply_predictor_inplace(std::vector<std::uint8_t>& raw, std::uint32_t width, std::uint32_t height,
                             std::uint16_t predictor) {
    constexpr std::uint32_t bps = 4;
    const std::uint32_t rowbytes = width * bps;
    if (predictor == 2) {
        // libtiff horizontal predictor on 32-bit samples: accumulate as uint32 words
        // (not classic per-byte differencing). Matches FABDEM / GDAL Float32 tiles.
        for (std::uint32_t y = 0; y < height; ++y) {
            auto* w = reinterpret_cast<std::uint32_t*>(raw.data() + y * rowbytes);
            for (std::uint32_t x = 1; x < width; ++x) w[x] += w[x - 1];
        }
        return;
    }
    if (predictor != 3) return;
    // Floating-point predictor (Adobe TIFF Technote 3): byte accumulate then unshuffle → BE floats.
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = raw.data() + y * rowbytes;
        for (std::uint32_t i = 1; i < rowbytes; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - 1]);
    }
    std::vector<std::uint8_t> tmp(raw.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = raw.data() + y * rowbytes;
        std::uint8_t* out = tmp.data() + y * rowbytes;
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t b = 0; b < bps; ++b) out[x * bps + b] = row[b * width + x];
    }
    raw.swap(tmp);
}

float read_be_float(const std::uint8_t* p) {
    const std::uint32_t u = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                            (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
    float f = 0.f;
    std::memcpy(&f, &u, 4);
    return f;
}

float read_le_float(const std::uint8_t* p) {
    float f = 0.f;
    std::memcpy(&f, p, 4);
    return f;
}

bool decode_tile(CogDem& dem, std::uint32_t tile_index, std::vector<float>& out_f32) {
    if (tile_index >= dem.tile_offsets.size()) {
        dem.err = "tile index OOB";
        return false;
    }
    const auto off = dem.tile_offsets[tile_index];
    const auto nbytes = dem.tile_byte_counts[tile_index];
    if (nbytes == 0) {
        out_f32.assign(static_cast<std::size_t>(dem.tile_w) * dem.tile_h, 0.f);
        return true;
    }
    std::vector<std::uint8_t> comp;
    if (!ensure_bytes(dem, off, nbytes, comp)) return false;
    const std::size_t expect = static_cast<std::size_t>(dem.tile_w) * dem.tile_h * 4u;
    std::vector<std::uint8_t> raw;
    if (!inflate_zlib(comp.data(), comp.size(), raw, expect)) {
        dem.err = "deflate tile failed";
        return false;
    }
    apply_predictor_inplace(raw, dem.tile_w, dem.tile_h, dem.predictor);
    out_f32.resize(static_cast<std::size_t>(dem.tile_w) * dem.tile_h);
    const bool be = (dem.predictor == 3);
    for (std::size_t i = 0; i < out_f32.size(); ++i) {
        const std::uint8_t* p = raw.data() + i * 4;
        out_f32[i] = be ? read_be_float(p) : read_le_float(p);
    }
    return true;
}

float sample_bilinear(CogDem& dem, double lat, double lon,
                      std::unordered_map<std::uint32_t, std::vector<float>>& tile_cache) {
    double col = (lon - dem.origin_lon) / dem.px_x;
    double row = (dem.origin_lat - lat) / dem.px_y;
    col = std::clamp(col, 0., static_cast<double>(dem.width - 1));
    row = std::clamp(row, 0., static_cast<double>(dem.height - 1));
    const int c0 = static_cast<int>(std::floor(col));
    const int r0 = static_cast<int>(std::floor(row));
    const int c1 = (std::min)(c0 + 1, static_cast<int>(dem.width - 1));
    const int r1 = (std::min)(r0 + 1, static_cast<int>(dem.height - 1));
    const float fx = static_cast<float>(col - c0);
    const float fy = static_cast<float>(row - r0);
    const std::uint32_t tiles_x = (dem.width + dem.tile_w - 1) / dem.tile_w;

    auto fetch_pix = [&](int c, int r) -> float {
        const std::uint32_t tx = static_cast<std::uint32_t>(c) / dem.tile_w;
        const std::uint32_t ty = static_cast<std::uint32_t>(r) / dem.tile_h;
        const std::uint32_t ti = ty * tiles_x + tx;
        auto it = tile_cache.find(ti);
        if (it == tile_cache.end()) {
            std::vector<float> tile;
            if (!decode_tile(dem, ti, tile)) return std::numeric_limits<float>::quiet_NaN();
            it = tile_cache.emplace(ti, std::move(tile)).first;
        }
        const std::uint32_t lx = static_cast<std::uint32_t>(c) % dem.tile_w;
        const std::uint32_t ly = static_cast<std::uint32_t>(r) % dem.tile_h;
        return it->second[static_cast<std::size_t>(ly) * dem.tile_w + lx];
    };

    const float z00 = fetch_pix(c0, r0);
    const float z10 = fetch_pix(c1, r0);
    const float z01 = fetch_pix(c0, r1);
    const float z11 = fetch_pix(c1, r1);
    if (!std::isfinite(z00) || !std::isfinite(z10) || !std::isfinite(z01) || !std::isfinite(z11))
        return std::numeric_limits<float>::quiet_NaN();
    const float z0 = z00 * (1.f - fx) + z10 * fx;
    const float z1 = z01 * (1.f - fx) + z11 * fx;
    return z0 * (1.f - fy) + z1 * fy;
}

std::string copernicus_cog_url(double lat, double lon) {
    const int lat_s = static_cast<int>(std::floor(lat));
    const int lon_w = static_cast<int>(std::floor(lon));
    const char ns = lat_s < 0 ? 'S' : 'N';
    const char ew = lon_w < 0 ? 'W' : 'E';
    char folder[128]{};
    std::snprintf(folder, sizeof(folder), "Copernicus_DSM_COG_10_%c%02d_00_%c%03d_00_DEM", ns, std::abs(lat_s),
                  ew, std::abs(lon_w));
    std::ostringstream q;
    q << "https://copernicus-dem-30m.s3.amazonaws.com/" << folder << '/' << folder << ".tif";
    return q.str();
}

void fabdem_names(double lat, double lon, std::string& zip_name, std::string& tif_name, std::string& cache_key) {
    const int lat_s = static_cast<int>(std::floor(lat));
    const int lon_w = static_cast<int>(std::floor(lon));
    char tile[64]{};
    std::snprintf(tile, sizeof(tile), "%c%02d%c%03d", lat_s < 0 ? 'S' : 'N', std::abs(lat_s),
                  lon_w < 0 ? 'W' : 'E', std::abs(lon_w));
    tif_name = std::string(tile) + "_FABDEM_V1-2.tif";
    cache_key = tile;

    const int lat10 = static_cast<int>(std::floor(lat / 10.)) * 10;
    const int lon10 = static_cast<int>(std::floor(lon / 10.)) * 10;
    const int lat10n = lat10 + 10;
    const int lon10n = lon10 + 10;
    char zsw[32]{}, zne[32]{};
    std::snprintf(zsw, sizeof(zsw), "%c%02d%c%03d", lat10 < 0 ? 'S' : 'N', std::abs(lat10),
                  lon10 < 0 ? 'W' : 'E', std::abs(lon10));
    std::snprintf(zne, sizeof(zne), "%c%02d%c%03d", lat10n < 0 ? 'S' : 'N', std::abs(lat10n),
                  lon10n < 0 ? 'W' : 'E', std::abs(lon10n));
    zip_name = std::string(zsw) + '-' + zne + "_FABDEM_V1-2.zip";
}

bool zip_http_extract_store(const std::wstring& zip_url, const std::string& member_name,
                            std::vector<std::uint8_t>& out_file, std::string& err) {
    const auto probe = http::http_get(zip_url, L"Range: bytes=0-0\r\n");
    if ((probe.status != 200 && probe.status != 206) || probe.content_length == 0) {
        err = "FABDEM zip size unknown HTTP " + std::to_string(probe.status);
        return false;
    }
    const std::uint64_t zip_size = probe.content_length;
    const std::uint64_t tail_from = zip_size > 65536 ? zip_size - 65536 : 0;
    std::vector<std::uint8_t> tail;
    if (!http_range(zip_url, tail_from, zip_size - 1, tail, err)) return false;

    int eocd = -1;
    for (int i = static_cast<int>(tail.size()) - 22; i >= 0; --i) {
        if (tail[static_cast<std::size_t>(i)] == 'P' && tail[static_cast<std::size_t>(i) + 1] == 'K' &&
            tail[static_cast<std::size_t>(i) + 2] == 5 && tail[static_cast<std::size_t>(i) + 3] == 6) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        err = "FABDEM zip EOCD not found";
        return false;
    }
    const std::uint8_t* e = tail.data() + eocd;
    const std::uint32_t cd_size = rd32(e + 12, true);
    const std::uint32_t cd_off = rd32(e + 16, true);
    std::vector<std::uint8_t> cd;
    if (!http_range(zip_url, cd_off, static_cast<std::uint64_t>(cd_off) + cd_size - 1, cd, err)) return false;

    std::uint32_t local_off = 0;
    std::uint32_t comp_size = 0;
    std::uint16_t method = 0xffff;
    bool found = false;
    std::size_t pos = 0;
    while (pos + 46 <= cd.size()) {
        if (cd[pos] != 'P' || cd[pos + 1] != 'K' || cd[pos + 2] != 1 || cd[pos + 3] != 2) break;
        method = rd16(cd.data() + pos + 10, true);
        comp_size = rd32(cd.data() + pos + 20, true);
        const std::uint16_t nlen = rd16(cd.data() + pos + 28, true);
        const std::uint16_t elen = rd16(cd.data() + pos + 30, true);
        const std::uint16_t clen = rd16(cd.data() + pos + 32, true);
        local_off = rd32(cd.data() + pos + 42, true);
        if (pos + 46 + nlen > cd.size()) break;
        const std::string name(reinterpret_cast<const char*>(cd.data() + pos + 46), nlen);
        pos += 46u + nlen + elen + clen;
        const bool match = (name == member_name) ||
                           (name.size() > member_name.size() &&
                            name.compare(name.size() - member_name.size(), member_name.size(), member_name) == 0 &&
                            (name[name.size() - member_name.size() - 1] == '/' ||
                             name[name.size() - member_name.size() - 1] == '\\'));
        if (match) {
            found = true;
            break;
        }
    }
    if (!found) {
        err = "FABDEM member not in zip: " + member_name;
        return false;
    }
    if (method != 0) {
        err = "FABDEM zip member not store (method=" + std::to_string(method) + ")";
        return false;
    }
    std::vector<std::uint8_t> lh;
    if (!http_range(zip_url, local_off, local_off + 30 + 512 - 1, lh, err)) return false;
    if (lh.size() < 30 || lh[0] != 'P' || lh[1] != 'K' || lh[2] != 3 || lh[3] != 4) {
        err = "FABDEM local header bad";
        return false;
    }
    const std::uint16_t nlen = rd16(lh.data() + 26, true);
    const std::uint16_t elen = rd16(lh.data() + 28, true);
    const std::uint64_t data_start = static_cast<std::uint64_t>(local_off) + 30u + nlen + elen;
    if (!http_range(zip_url, data_start, data_start + comp_size - 1, out_file, err)) return false;
    return true;
}

bool fabdem_ensure_local_tif(double lat, double lon, std::filesystem::path& out_path, std::string& err) {
    std::string zip_name, tif_name, cache_key;
    fabdem_names(lat, lon, zip_name, tif_name, cache_key);
    out_path = dem_cache_root() / "FABDEM_V1-2" / (cache_key + ".tif");
    std::error_code ec;
    if (std::filesystem::exists(out_path, ec) && std::filesystem::file_size(out_path, ec) > 1000) return true;

    const std::wstring zip_url =
        utf8_to_wide("https://data.bris.ac.uk/datasets/s5hqmjcdj8yo2ibzi9b4ew3sn/" + zip_name);
    std::vector<std::uint8_t> tif;
    if (!zip_http_extract_store(zip_url, tif_name, tif, err)) return false;
    if (tif.size() < 8 || tif[0] != 'I' || tif[1] != 'I') {
        err = "FABDEM extracted bytes are not TIFF";
        return false;
    }
    if (!write_local_file(out_path, tif)) {
        err = "FABDEM cache write failed";
        return false;
    }
    return true;
}

} // namespace

GeoJobResult sample_geotiff_float32_grid(const std::string& url_or_path, const TileWgs84Bounds& bounds,
                                         const int dim, std::vector<float>& out_heights, const char* label) {
    GeoJobResult r{};
    if (dim < 2) {
        r.message = "dim invalida";
        return r;
    }
    if (!wgs84_ok(bounds)) {
        r.message = "Bounds WGS84 invalidos";
        return r;
    }
    CogDem dem;
    if (!open_dem(dem, url_or_path)) {
        r.message = std::string(label) + ": " + dem.err;
        return r;
    }
    out_heights.assign(static_cast<std::size_t>(dim) * static_cast<std::size_t>(dim), 0.f);
    std::unordered_map<std::uint32_t, std::vector<float>> cache;
    cache.reserve(16);
    int bad = 0;
    for (int y = 0; y < dim; ++y) {
        const double fy = static_cast<double>(y) / static_cast<double>(dim - 1);
        const double lat = bounds.south + (bounds.north - bounds.south) * fy;
        for (int x = 0; x < dim; ++x) {
            const double fx = static_cast<double>(x) / static_cast<double>(dim - 1);
            const double lon = bounds.west + (bounds.east - bounds.west) * fx;
            const float z = sample_bilinear(dem, lat, lon, cache);
            if (!std::isfinite(z)) {
                ++bad;
                out_heights[static_cast<std::size_t>(y) * dim + x] = std::numeric_limits<float>::quiet_NaN();
            } else {
                out_heights[static_cast<std::size_t>(y) * dim + x] = z;
            }
        }
    }
    if (bad > dim * dim / 2) {
        r.message = std::string(label) + ": demasiados NaN (" + std::to_string(bad) + ") " + dem.err;
        return r;
    }
    fill_nans(out_heights, dim);
    r.ok = true;
    r.message = std::string(label) + " OK puntos=" + std::to_string(out_heights.size()) +
                " tiles=" + std::to_string(cache.size()) + " z0=" + std::to_string(out_heights.front()) + "m";
    return r;
}

GeoJobResult download_tile_dtm_copernicus_glo30(const TileWgs84Bounds& bounds, const int dim,
                                                std::vector<float>& out_heights) {
    const double mid_lat = (bounds.south + bounds.north) * 0.5;
    const double mid_lon = (bounds.west + bounds.east) * 0.5;
    return sample_geotiff_float32_grid(copernicus_cog_url(mid_lat, mid_lon), bounds, dim, out_heights,
                                       "Copernicus GLO-30");
}

GeoJobResult download_tile_dtm_fabdem(const TileWgs84Bounds& bounds, const int dim,
                                      std::vector<float>& out_heights) {
    GeoJobResult r{};
    const double mid_lat = (bounds.south + bounds.north) * 0.5;
    const double mid_lon = (bounds.west + bounds.east) * 0.5;
    std::filesystem::path local;
    std::string err;
    if (!fabdem_ensure_local_tif(mid_lat, mid_lon, local, err)) {
        r.message = "FABDEM: " + err;
        return r;
    }
    return sample_geotiff_float32_grid(local.string(), bounds, dim, out_heights, "FABDEM V1-2");
}

} // namespace omsi::map
