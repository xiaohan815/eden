// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "shader_recompiler/shader_info.h"

namespace Shader {

constexpr SampledType GetSampledType(TexturePixelFormat format) {
    switch (format) {
    case TexturePixelFormat::A8B8G8R8_SINT:
    case TexturePixelFormat::R8_SINT:
    case TexturePixelFormat::R16G16B16A16_SINT:
    case TexturePixelFormat::R32G32B32A32_SINT:
    case TexturePixelFormat::R32G32_SINT:
    case TexturePixelFormat::R16_SINT:
    case TexturePixelFormat::R16G16_SINT:
    case TexturePixelFormat::R8G8_SINT:
    case TexturePixelFormat::R32_SINT:
        return SampledType::SignedInt;
    case TexturePixelFormat::A8B8G8R8_UINT:
    case TexturePixelFormat::A2B10G10R10_UINT:
    case TexturePixelFormat::R8_UINT:
    case TexturePixelFormat::R16G16B16A16_UINT:
    case TexturePixelFormat::R32G32B32A32_UINT:
    case TexturePixelFormat::R16_UINT:
    case TexturePixelFormat::R16G16_UINT:
    case TexturePixelFormat::R8G8_UINT:
    case TexturePixelFormat::R32G32_UINT:
    case TexturePixelFormat::R32_UINT:
    case TexturePixelFormat::S8_UINT:
        return SampledType::UnsignedInt;
    default:
        // Normalized, floating-point, compressed and depth formats return floats.
        return SampledType::Float;
    }
}

template <typename Descriptor, typename ReadCbuf>
u32 ReadTextureHandle(const Descriptor& desc, u32 element, const ReadCbuf& read_cbuf) {
    const u32 offset = element << desc.size_shift;
    const u32 primary = read_cbuf(desc.cbuf_index, desc.cbuf_offset + offset);
    if constexpr (requires { desc.has_secondary; }) {
        if (desc.has_secondary) {
            return (primary << desc.shift_left) |
                   (read_cbuf(desc.secondary_cbuf_index, desc.secondary_cbuf_offset + offset)
                    << desc.secondary_shift_left);
        }
    }
    return primary;
}

// Vulkan image arrays have one scalar type. Check every element, including dynamic arrays,
// before compiling or reusing a pipeline. A different scalar type needs a separate variant.
template <typename ReadCbuf, typename ReadFormat>
bool TextureTypesMatch(const Info& info, const ReadCbuf& read_cbuf, const ReadFormat& read_format) {
    const auto sampled_match = [&](const auto& descriptors) {
        for (const auto& desc : descriptors) {
            for (u32 element = 0; element < desc.count; ++element) {
                if (GetSampledType(read_format(ReadTextureHandle(desc, element, read_cbuf))) !=
                    desc.sampled_type) {
                    return false;
                }
            }
        }
        return true;
    };
    const auto image_match = [&](const auto& descriptors) {
        for (const auto& desc : descriptors) {
            // Explicit formats are encoded in the guest instruction, independent of the TIC.
            if (desc.format != ImageFormat::Typeless) {
                continue;
            }
            const auto expected = desc.is_signed ? SampledType::SignedInt
                                : desc.is_integer ? SampledType::UnsignedInt
                                                  : SampledType::Float;
            for (u32 element = 0; element < desc.count; ++element) {
                if (GetSampledType(read_format(ReadTextureHandle(desc, element, read_cbuf))) !=
                    expected) {
                    return false;
                }
            }
        }
        return true;
    };
    return sampled_match(info.texture_buffer_descriptors) &&
           sampled_match(info.texture_descriptors) && image_match(info.image_buffer_descriptors) &&
           image_match(info.image_descriptors);
}

} // namespace Shader
