// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/texture_types.h"

namespace {
using namespace Shader;

struct Bindings {
    std::array<std::array<u32, 8>, 2> cbufs{};
    std::array<TexturePixelFormat, 8> formats{};

    bool Matches(const Info& info) const {
        return TextureTypesMatch(
            info, [&](u32 bank, u32 offset) { return cbufs.at(bank).at(offset / 4); },
            [&](u32 handle) { return formats.at(handle); });
    }
};
} // namespace

TEST_CASE("Texture pipeline variants follow bound scalar types", "[shader][texture]") {
    Info float_variant;
    float_variant.texture_descriptors.push_back({
        .sampled_type = SampledType::Float,
        .cbuf_index = 0,
        .cbuf_offset = 0,
        .count = 1,
        .size_shift = 3,
    });
    Info signed_variant = float_variant;
    signed_variant.texture_descriptors[0].sampled_type = SampledType::SignedInt;
    Info unsigned_variant = float_variant;
    unsigned_variant.texture_descriptors[0].sampled_type = SampledType::UnsignedInt;
    Bindings bindings;
    for (const auto format : {TexturePixelFormat::R8_UNORM, TexturePixelFormat::R8_SNORM,
                              TexturePixelFormat::R32_FLOAT, TexturePixelFormat::BC7_UNORM,
                              TexturePixelFormat::D32_FLOAT_S8_UINT}) {
        bindings.formats[0] = format;
        CHECK(bindings.Matches(float_variant));
        CHECK_FALSE(bindings.Matches(signed_variant));
        CHECK_FALSE(bindings.Matches(unsigned_variant));
    }
    bindings.formats[0] = TexturePixelFormat::R32_SINT;
    CHECK_FALSE(bindings.Matches(float_variant));
    CHECK(bindings.Matches(signed_variant));
    CHECK_FALSE(bindings.Matches(unsigned_variant));
    bindings.formats[0] = TexturePixelFormat::R32_UINT;
    CHECK_FALSE(bindings.Matches(float_variant));
    CHECK_FALSE(bindings.Matches(signed_variant));
    CHECK(bindings.Matches(unsigned_variant));
    // Changing the handle must re-check its format, even if the shader is unchanged.
    bindings.cbufs[0][0] = 1;
    bindings.formats[1] = TexturePixelFormat::R16G16B16A16_FLOAT;
    CHECK(bindings.Matches(float_variant));
    CHECK_FALSE(bindings.Matches(unsigned_variant));
}

TEST_CASE("Dynamic and separate-sampler textures check every element", "[shader][texture]") {
    Info info;
    info.texture_descriptors.push_back({
        .sampled_type = SampledType::UnsignedInt,
        .has_secondary = true,
        .cbuf_index = 0,
        .cbuf_offset = 0,
        .shift_left = 1,
        .secondary_cbuf_index = 1,
        .secondary_cbuf_offset = 4,
        .secondary_shift_left = 0,
        .count = 2,
        .size_shift = 3,
    });
    Bindings bindings;
    bindings.cbufs[0][0] = 1;
    bindings.cbufs[1][1] = 1;
    bindings.cbufs[0][2] = 2;
    bindings.cbufs[1][3] = 1;
    bindings.formats[3] = TexturePixelFormat::R8_UINT;
    bindings.formats[5] = TexturePixelFormat::R16G16_UINT;
    CHECK(bindings.Matches(info));
    bindings.formats[5] = TexturePixelFormat::R16G16_SINT;
    CHECK_FALSE(bindings.Matches(info));
    bindings.formats[5] = TexturePixelFormat::R16G16_UNORM;
    CHECK_FALSE(bindings.Matches(info));
}

TEST_CASE("Storage and buffer texture variants retain signedness", "[shader][texture]") {
    Bindings bindings;
    bindings.formats[0] = TexturePixelFormat::R8_SINT;
    Info info;
    SECTION("sampled buffer") {
        info.texture_buffer_descriptors.push_back({
            .sampled_type = SampledType::SignedInt, .count = 1, .size_shift = 3});
    }
    SECTION("storage buffer") {
        info.image_buffer_descriptors.push_back({
            .format = ImageFormat::Typeless, .is_integer = true, .is_signed = true,
            .count = 1, .size_shift = 3});
    }
    SECTION("storage texture") {
        info.image_descriptors.push_back({
            .format = ImageFormat::Typeless, .is_integer = true, .is_signed = true,
            .count = 1, .size_shift = 3});
    }
    CHECK(bindings.Matches(info));
    bindings.formats[0] = TexturePixelFormat::R8_UINT;
    CHECK_FALSE(bindings.Matches(info));
    bindings.formats[0] = TexturePixelFormat::R8_SNORM;
    CHECK_FALSE(bindings.Matches(info));
}

TEST_CASE("Explicit storage formats do not specialize on the TIC", "[shader][texture]") {
    Info info;
    info.image_descriptors.push_back({
        .format = ImageFormat::R16_SINT, .is_integer = true, .is_signed = true,
        .count = 1, .size_shift = 3});
    bool read{};
    const auto matches = TextureTypesMatch(
        info, [&](u32, u32) -> u32 { read = true; return 0; },
        [&](u32) { read = true; return TexturePixelFormat::R8_UNORM; });
    CHECK(matches);
    CHECK_FALSE(read);
}
