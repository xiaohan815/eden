// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "shader_recompiler/environment.h"

namespace Shader {

// Inputs read while translating a shader. A failed translation is reusable only
// while these inputs still match; another binding can produce a valid variant.
struct Specialization {
    Stage stage{};
    u32 texture_bound{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};
    std::array<u32, 8> gp_passthrough_mask{};
    bool has_hle_engine_state{};
    std::optional<u32> viewport_transform_state;
    std::vector<std::pair<u64, u32>> cbuf_values;
    std::vector<std::pair<u32, TextureType>> texture_types;
    std::vector<std::pair<u32, TexturePixelFormat>> texture_pixel_formats;
    std::vector<std::pair<u64, std::optional<ReplaceConstant>>> cbuf_replacements;

    template <typename CbufEnabled, typename TextureAccessible>
    bool Matches(Environment& env, const CbufEnabled& cbuf_enabled,
                 const TextureAccessible& texture_accessible) const {
        if (stage != env.ShaderStage() || texture_bound != env.TextureBoundBuffer() ||
            local_memory_size != env.LocalMemorySize() ||
            shared_memory_size != env.SharedMemorySize() || workgroup_size != env.WorkgroupSize() ||
            gp_passthrough_mask != env.GpPassthroughMask() ||
            has_hle_engine_state != env.HasHLEMacroState() ||
            (viewport_transform_state &&
             *viewport_transform_state != env.ReadViewportTransformState())) {
            return false;
        }
        for (const auto& [key, value] : cbuf_values) {
            const auto bank = static_cast<u32>(key >> 32);
            const auto offset = static_cast<u32>(key);
            // Do not call ReadCbufValue on a disabled binding: it asserts.
            if (!cbuf_enabled(bank) || env.ReadCbufValue(bank, offset) != value) {
                return false;
            }
        }
        for (const auto& [handle, type] : texture_types) {
            if (!texture_accessible(handle) || env.ReadTextureType(handle) != type) {
                return false;
            }
        }
        for (const auto& [handle, format] : texture_pixel_formats) {
            if (!texture_accessible(handle) || env.ReadTexturePixelFormat(handle) != format) {
                return false;
            }
        }
        for (const auto& [key, replacement] : cbuf_replacements) {
            if (env.GetReplaceConstBuffer(static_cast<u32>(key >> 32), static_cast<u32>(key)) !=
                replacement) {
                return false;
            }
        }
        return true;
    }
};

} // namespace Shader
