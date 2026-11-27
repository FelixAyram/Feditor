#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace omsi::gfx::dx12 {

Dx12Texture load_texture_wic(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                             const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                             const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, const std::filesystem::path& path) {
    Dx12Texture out{};
    if (!device || !cmd_list) return out;
    if (const auto cached = texture_detail::cpu_mip_cache_take(path)) {
        return texture_detail::upload_rgba_mip_chain(device, cmd_list, srv_cpu, srv_gpu, *cached);
    }
    if (!texture_detail::path_exists(path)) return out;
    // (a 32-bit BMP keeps its alpha, as D3DX reads it: WIC would make it opaque)
    if (!is_virtual_path(path) && texture_detail::sniff_texture_format(path) == texture_detail::TexFormat::bmp)
        if (const auto img = texture_detail::load_bmp32_alpha_cpu(path))
            return texture_detail::upload_rgba_texture(device, cmd_list, srv_cpu, srv_gpu, img->rgba.data(), img->width,
                                                       img->height, img->width * 4u);

    // WIC needs COM on this thread (editor uploads after init may ride a different apartment).
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> factory;
    throw_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
             "WIC factory");

    ComPtr<IWICBitmapDecoder> decoder;
    std::vector<std::uint8_t> vbytes;
    ComPtr<IWICStream> vstream;
    if (is_virtual_path(path) || omsi::vfs::get(path)) {
        if (!texture_detail::read_file_bytes(path, vbytes)) return out;
        throw_hr(factory->CreateStream(vstream.GetAddressOf()), "WIC stream");
        throw_hr(vstream->InitializeFromMemory(vbytes.data(), static_cast<DWORD>(vbytes.size())), "WIC stream init");
        throw_hr(factory->CreateDecoderFromStream(vstream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()),
                 "WIC decoder (vfs)");
    } else {
        throw_hr(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                    decoder.GetAddressOf()),
                 "WIC decoder");
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    throw_hr(decoder->GetFrame(0, frame.GetAddressOf()), "WIC frame");

    ComPtr<IWICFormatConverter> converter;
    throw_hr(factory->CreateFormatConverter(converter.GetAddressOf()), "WIC converter");
    throw_hr(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.f,
                                   WICBitmapPaletteTypeCustom),
             "WIC convert");

    UINT width = 0;
    UINT height = 0;
    throw_hr(converter->GetSize(&width, &height), "WIC size");
    if (width == 0 || height == 0) return out;

    const UINT stride = width * 4;
    const UINT image_size = stride * height;
    std::vector<std::uint8_t> pixels(image_size);
    throw_hr(converter->CopyPixels(nullptr, stride, image_size, pixels.data()), "WIC copy");

    return texture_detail::upload_rgba_texture(device, cmd_list, srv_cpu, srv_gpu, pixels.data(), width, height, stride);
}
} // namespace omsi::gfx::dx12
