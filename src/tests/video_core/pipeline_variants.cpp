// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/texture_types.h"
#include "video_core/pipeline_variants.h"
#include "video_core/shader_environment.h"

namespace {
using namespace Shader;

struct Bindings {
    std::array<u32, 4> handles{0, 0, 1, 0};
    std::array<TexturePixelFormat, 4> formats{TexturePixelFormat::R32_SINT,
                                          TexturePixelFormat::R32_UINT};
    TextureType type = TextureType::Color2D;
    std::optional<ReplaceConstant> replacement;
    u32 viewport = 1;
    u32 texture_limit = 3;
    bool enabled = true;
};

class TestEnvironment final : public VideoCommon::GenericEnvironment {
public:
    explicit TestEnvironment(Bindings& bindings_) : bindings{bindings_} {
        stage = Stage::Fragment;
    }

    u32 ReadCbufValue(u32 bank, u32 offset) override {
        REQUIRE(bindings.enabled);
        const u32 value = bindings.handles.at(offset / 4);
        cbuf_values.emplace((static_cast<u64>(bank) << 32) | offset, value);
        return value;
    }
    TextureType ReadTextureType(u32 handle) override {
        REQUIRE(handle <= bindings.texture_limit);
        texture_types.emplace(handle, bindings.type);
        return bindings.type;
    }
    TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        REQUIRE(handle <= bindings.texture_limit);
        const auto value = bindings.formats.at(handle);
        texture_pixel_formats.emplace(handle, value);
        return value;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return GetSampledType(ReadTexturePixelFormat(handle)) != SampledType::Float;
    }
    u32 ReadViewportTransformState() override {
        viewport_transform_read = true;
        viewport_transform_state = bindings.viewport;
        return viewport_transform_state;
    }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        cbuf_replacement_queries.emplace((static_cast<u64>(bank) << 32) | offset,
                                         bindings.replacement);
        return bindings.replacement;
    }
    bool Matches(const Specialization& failure) {
        return failure.Matches(*this, [&](u32) { return bindings.enabled; },
                               [&](u32 handle) { return handle <= bindings.texture_limit; });
    }

private:
    Bindings& bindings;
};

Info ArrayInfo(SampledType type) {
    Info info;
    info.texture_descriptors.push_back({.sampled_type = type, .count = 2, .size_shift = 3});
    return info;
}

bool MatchesTextures(const Info& info, TestEnvironment& env) {
    return TextureTypesMatch(info, [&](u32 bank, u32 offset) { return env.ReadCbufValue(bank, offset); },
                             [&](u32 handle) { return env.ReadTexturePixelFormat(handle); });
}
} // namespace

TEST_CASE("Failed texture specialization recovers without recompiling unchanged bindings",
          "[shader][pipeline]") {
    VideoCommon::PipelineVariants<Info, Specialization> variants;
    Bindings bindings;
    int builds{};
    const auto lookup = [&]() -> Info* {
        TestEnvironment live{bindings};
        const auto cached = variants.Find(
            [&](const Info& info) { return MatchesTextures(info, live); },
            [&](const Specialization& failure) { return live.Matches(failure); });
        if (cached) {
            return *cached;
        }
        ++builds;
        TestEnvironment compile{bindings};
        auto info = std::make_unique<Info>(ArrayInfo(GetSampledType(bindings.formats[0])));
        if (!MatchesTextures(*info, compile)) {
            variants.failures.push_back(compile.GetSpecialization());
            return nullptr;
        }
        auto* result = info.get();
        variants.pipelines.push_back(std::move(info));
        return result;
    };

    CHECK(lookup() == nullptr); // Mixed signed / unsigned array is unsupported.
    CHECK(lookup() == nullptr);
    CHECK(builds == 1);
    bindings.formats[0] = TexturePixelFormat::R32_UINT;
    auto* unsigned_pipeline = lookup();
    REQUIRE(unsigned_pipeline != nullptr);
    CHECK(builds == 2);
    CHECK(lookup() == unsigned_pipeline);
    CHECK(builds == 2);
    bindings.formats[0] = bindings.formats[1] = TexturePixelFormat::R32_FLOAT;
    auto* float_pipeline = lookup();
    REQUIRE(float_pipeline != nullptr);
    CHECK(float_pipeline != unsigned_pipeline);
    CHECK(builds == 3);
    bindings.formats[0] = bindings.formats[1] = TexturePixelFormat::R32_UINT;
    CHECK(lookup() == unsigned_pipeline);
    CHECK(builds == 3); // Successful variants and their addresses survive another build.
    bindings.formats[0] = TexturePixelFormat::R32_SINT;
    CHECK(lookup() == nullptr);
    CHECK(builds == 3); // The original failed binding still does not retry every draw.
}

TEST_CASE("Failed shader dependencies check handles, formats and disabled buffers",
          "[shader][pipeline]") {
    Bindings bindings;
    TestEnvironment compile{bindings};
    CHECK_FALSE(MatchesTextures(ArrayInfo(SampledType::SignedInt), compile));
    const auto failure = compile.GetSpecialization();
    const auto matches = [&] { TestEnvironment live{bindings}; return live.Matches(failure); };
    CHECK(matches());
    SECTION("same scalar type with a new handle") {
        bindings.formats[2] = bindings.formats[1];
        bindings.handles[2] = 2;
        CHECK_FALSE(matches());
    }
    SECTION("format changes with the handle unchanged") {
        bindings.formats[1] = TexturePixelFormat::R16_UINT;
        CHECK_FALSE(matches());
    }
    SECTION("disabled buffer must not be read") {
        bindings.enabled = false;
        CHECK_FALSE(matches());
    }
    SECTION("texture table was shortened") {
        bindings.texture_limit = 0;
        CHECK_FALSE(matches());
    }
}

TEST_CASE("Failed shader dependencies preserve queried state and absent replacements",
          "[shader][pipeline]") {
    Bindings bindings;
    TestEnvironment compile{bindings};
    compile.ReadTextureType(0);
    compile.ReadViewportTransformState();
    compile.GetReplaceConstBuffer(0, 0);
    const auto failure = compile.GetSpecialization();
    TestEnvironment live{bindings};
    CHECK(live.Matches(failure));
    SECTION("texture dimension") { bindings.type = TextureType::Color3D; }
    SECTION("viewport transform") { bindings.viewport = 0; }
    SECTION("previously absent replacement") { bindings.replacement = ReplaceConstant::DrawID; }
    CHECK_FALSE(live.Matches(failure));
}

TEST_CASE("Failed cache entries cannot hide a valid pipeline variant", "[shader][pipeline]") {
    VideoCommon::PipelineVariants<int, int> variants;
    variants.pipelines.push_back(nullptr);
    variants.pipelines.push_back(std::make_unique<int>(42));
    variants.failures.push_back(42);
    const auto hit = variants.Find([](int value) { return value == 42; }, [](int) { return true; });
    REQUIRE(hit.has_value());
    CHECK(**hit == 42);
    const auto failure = variants.Find([](int) { return false; }, [](int) { return true; });
    REQUIRE(failure.has_value());
    CHECK(*failure == nullptr);
}
