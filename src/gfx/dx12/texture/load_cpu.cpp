#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace omsi::gfx::dx12 {
namespace {

std::optional<CpuRgbaImage> load_wic_cpu(const std::filesystem::path& path) {
    if (!texture_detail::path_exists(path)) return std::nullopt;
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory* factory_raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_raw))))
        return std::nullopt;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory(factory_raw);

    IWICBitmapDecoder* decoder_raw = nullptr;
    std::vector<std::uint8_t> ram_bytes;
    Microsoft::WRL::ComPtr<IWICStream> ram_stream;
    if (omsi::vfs::get(path)) {
        // co-op: the file only exists in RAM
        if (!texture_detail::read_file_bytes(path, ram_bytes)) return std::nullopt;
        if (FAILED(factory->CreateStream(ram_stream.GetAddressOf())) ||
            FAILED(ram_stream->InitializeFromMemory(ram_bytes.data(), static_cast<DWORD>(ram_bytes.size()))) ||
            FAILED(factory->CreateDecoderFromStream(ram_stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder_raw)))
            return std::nullopt;
    } else if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnLoad, &decoder_raw)))
        return std::nullopt;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder(decoder_raw);

    IWICBitmapFrameDecode* frame_raw = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame_raw))) return std::nullopt;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame(frame_raw);

    IWICFormatConverter* conv_raw = nullptr;
    if (FAILED(factory->CreateFormatConverter(&conv_raw))) return std::nullopt;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter(conv_raw);
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.f,
                                     WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return std::nullopt;

    const UINT stride = width * 4u;
    const UINT size = stride * height;
    CpuRgbaImage out{};
    out.width = width;
    out.height = height;
    out.rgba.resize(size);
    if (FAILED(converter->CopyPixels(nullptr, stride, size, out.rgba.data()))) return std::nullopt;
    return out;
}

std::optional<CpuRgbaImage> load_dds_cpu(const std::filesystem::path& path) {
    if (!texture_detail::path_exists(path)) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    if (!texture_detail::read_file_bytes(path, bytes)) return std::nullopt;
    if (bytes.size() < 128u || std::memcmp(bytes.data(), "DDS ", 4) != 0) return std::nullopt;
    {
        CpuRgbaImage img{};
        if (texture_detail::dds_uncompressed_to_rgba(bytes.data(), bytes.size(), img.rgba, img.width, img.height)) return img;
    }

    const auto* hdr = bytes.data();
    const std::uint32_t height = *reinterpret_cast<const std::uint32_t*>(hdr + 12);
    const std::uint32_t width = *reinterpret_cast<const std::uint32_t*>(hdr + 16);
    const std::uint32_t pitch = *reinterpret_cast<const std::uint32_t*>(hdr + 20);
    const std::uint32_t fourcc = *reinterpret_cast<const std::uint32_t*>(hdr + 84);
    const std::uint32_t rgb_bitcount = *reinterpret_cast<const std::uint32_t*>(hdr + 88);
    if (width == 0 || height == 0) return std::nullopt;

    const DXGI_FORMAT format = texture_detail::dds_dxgi_format(fourcc, rgb_bitcount);
    if (format == DXGI_FORMAT_UNKNOWN) return std::nullopt;

    const std::uint32_t computed_size = texture_detail::dds_top_mip_size(width, height, format, pitch);
    const std::uint32_t available = static_cast<std::uint32_t>(bytes.size() - 128u);
    if (computed_size == 0 || computed_size > available) return std::nullopt;
    const std::uint8_t* pixel_data = bytes.data() + 128;

    CpuRgbaImage out{};
    out.width = width;
    out.height = height;
    if (texture_detail::is_block_compressed(format)) {
        out.rgba = texture_detail::decompress_dds_blocks(pixel_data, width, height, format);
    } else if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        const std::uint32_t src_stride = pitch > 0 ? pitch : width * 4u;
        out.rgba.resize(width * height * 4u);
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(out.rgba.data() + y * width * 4u, pixel_data + y * src_stride, width * 4u);
    } else {
        return std::nullopt;
    }
    return out;
}

} // namespace

namespace texture_detail {
std::optional<CpuRgbaImage> load_bmp32_alpha_cpu(const std::filesystem::path& path) {
    std::vector<std::uint8_t> b;
    if (!read_file_bytes(path, b) || b.size() < 54u || b[0] != 'B' || b[1] != 'M') return std::nullopt;
    auto u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8)); };
    auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    const std::uint32_t off = u32(10), hdr = u32(14), bpp = u16(28), comp = u32(30);
    const std::int32_t w = static_cast<std::int32_t>(u32(18)), h_raw = static_cast<std::int32_t>(u32(22));
    if (hdr < 40u || bpp != 32u || w <= 0 || h_raw == 0 || w > 16384) return std::nullopt;
    // channel masks: BI_RGB = B G R A bytes; BI_BITFIELDS / ALPHABITFIELDS with the usual BGRA masks only
    if (comp == 3u || comp == 6u) {
        if (b.size() < 54u + 12u) return std::nullopt;
        const std::uint32_t rm = u32(54), gm = u32(58), bm = u32(62);
        if (rm != 0x00FF0000u || gm != 0x0000FF00u || bm != 0x000000FFu) return std::nullopt;
    } else if (comp != 0u) {
        return std::nullopt;
    }
    const bool top_down = h_raw < 0;
    const std::uint32_t h = static_cast<std::uint32_t>(top_down ? -h_raw : h_raw);
    if (h > 16384u) return std::nullopt;
    const std::size_t stride = static_cast<std::size_t>(w) * 4u;
    if (off > b.size() || b.size() - off < stride * h) return std::nullopt;
    CpuRgbaImage out{};
    out.width = static_cast<std::uint32_t>(w);
    out.height = h;
    out.rgba.resize(stride * h);
    bool any_alpha = false;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* s = b.data() + off + stride * (top_down ? y : h - 1u - y);
        std::uint8_t* d = out.rgba.data() + stride * y;
        for (std::int32_t x = 0; x < w; ++x, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
            any_alpha |= s[3] != 0;
        }
    }
    if (!any_alpha) return std::nullopt;
    return out;
}
} // namespace texture_detail

std::optional<CpuRgbaImage> load_image_cpu_rgba(const std::filesystem::path& path) {
    if (!is_virtual_path(path)) {
        using F = texture_detail::TexFormat;
        switch (texture_detail::sniff_texture_format(path)) {
        case F::dds:
            if (auto img = load_dds_cpu(path)) return img;
            break;
        case F::bmp:
            if (auto img = texture_detail::load_bmp32_alpha_cpu(path)) return img;
            if (auto img = load_wic_cpu(path)) return img;
            break;
        case F::png:
        case F::jpg:
            if (auto img = load_wic_cpu(path)) return img;
            break;
        case F::tga:
            if (!texture_detail::iequals_ext(path, L".dds"))
                if (auto img = texture_detail::load_tga_cpu(path)) return img;
            break;
        default: break;
        }
    }
    if (texture_detail::iequals_ext(path, L".tga")) {
        if (auto img = texture_detail::load_tga_cpu(path)) return img;
    }
    if (texture_detail::iequals_ext(path, L".dds")) {
        if (auto img = load_dds_cpu(path)) return img;
    }
    if (auto img = load_wic_cpu(path)) return img;
    if (!texture_detail::iequals_ext(path, L".dds")) {
        const auto dds = path.parent_path() / (path.stem().wstring() + L".dds");
        if (texture_detail::path_exists(dds)) return load_dds_cpu(dds);
    }
    return std::nullopt;
}

} // namespace omsi::gfx::dx12
