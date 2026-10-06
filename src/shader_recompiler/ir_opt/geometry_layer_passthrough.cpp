// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
struct Scalar {
    u32 number{};
    std::optional<IR::Attribute> attribute;
};

struct Passthrough {
    IR::Attribute layer_attribute;
    VaryingState stores;
};

bool IsVertexAttribute(IR::Attribute attr) {
    return (attr >= IR::Attribute::PositionX && attr <= IR::Attribute::PositionW) ||
           (attr >= IR::Attribute::Generic0X && attr <= IR::Attribute::Generic31W);
}

// Interpret only constant control flow and copies of input attributes. Any unknown operation,
// vertex reordering, stream, memory side effect, or generated primitive rejects the lowering.
class PassthroughProof {
public:
    std::optional<Passthrough> Run(const IR::Program& program) {
        std::vector<bool> active{true};
        for (const auto& node : program.syntax_list) {
            using Type = IR::AbstractSyntaxNode::Type;
            switch (node.type) {
            case Type::If: {
                const auto cond = active.back() ? Number(node.data.if_node.cond)
                                                : std::optional<u32>{0};
                if (!cond) {
                    return {};
                }
                active.push_back(active.back() && *cond != 0);
                break;
            }
            case Type::EndIf:
                if (active.size() == 1) {
                    return {};
                }
                active.pop_back();
                break;
            case Type::Block:
                if (active.back()) {
                    for (const IR::Inst& inst : node.data.block->Instructions()) {
                        if (!Execute(inst)) {
                            return {};
                        }
                    }
                    previous_block = node.data.block;
                }
                break;
            case Type::Return:
                if (active.back()) {
                    return Result();
                }
                break;
            default:
                // Loops and conditional exits require a more general geometry implementation.
                return {};
            }
        }
        return active.size() == 1 ? Result() : std::nullopt;
    }

private:
    std::optional<Passthrough> Result() const {
        if (emitted != 3 || !layer_attribute) {
            return {};
        }
        VaryingState stores;
        for (const auto attr : first_outputs) {
            stores.Set(attr);
        }
        return Passthrough{*layer_attribute, stores};
    }

    std::optional<Scalar> Value(IR::Value value) const {
        value = value.Resolve();
        if (value.IsImmediate()) {
            if (value.Type() == IR::Type::U32) {
                return Scalar{value.U32(), {}};
            }
            if (value.Type() == IR::Type::U1) {
                return Scalar{value.U1() ? 1u : 0u, {}};
            }
            return {};
        }
        const auto found = values.find(value.Inst());
        return found == values.end() ? std::nullopt : std::optional{found->second};
    }

    std::optional<u32> Number(IR::Value value) const {
        const auto scalar = Value(value);
        return scalar && !scalar->attribute ? std::optional{scalar->number} : std::nullopt;
    }

    bool Execute(const IR::Inst& inst) {
        using Op = IR::Opcode;
        std::optional<Scalar> result;
        switch (inst.GetOpcode()) {
        case Op::Prologue:
        case Op::Epilogue:
        case Op::Void:
            return true;
        case Op::Identity:
        case Op::ConditionRef:
            result = Value(inst.Arg(0));
            break;
        case Op::Phi:
            for (size_t i = 0; i < inst.NumArgs(); ++i) {
                if (inst.PhiBlock(i) == previous_block) {
                    result = Value(inst.Arg(i));
                    break;
                }
            }
            break;
        case Op::InvocationInfo:
            result = Scalar{3u << 16, {}};
            break;
        case Op::BitFieldUExtract: {
            const auto base = Number(inst.Arg(0));
            const auto offset = Number(inst.Arg(1));
            const auto count = Number(inst.Arg(2));
            if (base && offset && count && *offset < 32 && *count <= 32 - *offset) {
                const u32 mask = *count == 32 ? ~0u : (1u << *count) - 1;
                result = Scalar{(*base >> *offset) & mask, {}};
            }
            break;
        }
        case Op::IMul32:
        case Op::IAdd32:
        case Op::ULessThanEqual: {
            const auto a = Number(inst.Arg(0));
            const auto b = Number(inst.Arg(1));
            if (a && b) {
                const u32 number = inst.GetOpcode() == Op::IMul32 ? *a * *b
                                 : inst.GetOpcode() == Op::IAdd32 ? *a + *b
                                                                 : u32(*a <= *b);
                result = Scalar{number, {}};
            }
            break;
        }
        case Op::GetAttribute: {
            const auto attr = inst.Arg(0).Attribute();
            const auto vertex = Number(inst.Arg(1));
            if (vertex && *vertex < 3 && IsVertexAttribute(attr)) {
                result = Scalar{*vertex, attr};
            }
            break;
        }
        case Op::SetAttribute: {
            const auto attr = inst.Arg(0).Attribute();
            const auto value = Value(inst.Arg(1));
            const auto vertex = Number(inst.Arg(2));
            if (!value || !value->attribute || !vertex || *vertex != 0 ||
                (!IsVertexAttribute(attr) && attr != IR::Attribute::Layer)) {
                return false;
            }
            outputs.insert_or_assign(attr, *value);
            return true;
        }
        case Op::EmitVertex: {
            if (Number(inst.Arg(0)) != 0 || emitted >= 3) {
                return false;
            }
            for (u32 i = 0; i < 4; ++i) {
                if (!outputs.contains(IR::Attribute::PositionX + i)) {
                    return false;
                }
            }
            if (!outputs.contains(IR::Attribute::Layer)) {
                return false;
            }
            std::vector<IR::Attribute> written;
            for (const auto& [attr, value] : outputs) {
                written.push_back(attr);
            }
            if (emitted != 0 && written != first_outputs) {
                return false;
            }
            first_outputs = std::move(written);
            for (const auto& [attr, value] : outputs) {
                if (value.number != emitted) {
                    return false;
                }
                if (attr == IR::Attribute::Layer) {
                    if (*value.attribute < IR::Attribute::Generic0X ||
                        *value.attribute > IR::Attribute::Generic31W ||
                        (layer_attribute && layer_attribute != value.attribute)) {
                        return false;
                    }
                    layer_attribute = value.attribute;
                } else if (value.attribute != attr) {
                    return false;
                }
            }
            ++emitted;
            // Geometry outputs become undefined after emission; require each vertex's writes.
            outputs.clear();
            return true;
        }
        case Op::EndPrimitive:
            return Number(inst.Arg(0)) == 0 && emitted == 3;
        default:
            return false;
        }
        if (!result) {
            return false;
        }
        values.insert_or_assign(&inst, *result);
        return true;
    }

    std::unordered_map<const IR::Inst*, Scalar> values;
    std::map<IR::Attribute, Scalar> outputs;
    std::vector<IR::Attribute> first_outputs;
    const IR::Block* previous_block{};
    std::optional<IR::Attribute> layer_attribute;
    u32 emitted{};
};
} // namespace

bool LowerGeometryLayerPassthrough(IR::Program& vertex, const IR::Program& geometry) {
    if (vertex.stage != Stage::VertexB || vertex.info.stores[IR::Attribute::Layer] ||
        geometry.stage != Stage::Geometry || geometry.invocations != 1 ||
        geometry.output_vertices != 3 || geometry.output_topology != OutputTopology::TriangleStrip) {
        return false;
    }
    const auto passthrough = PassthroughProof{}.Run(geometry);
    if (!passthrough || !vertex.info.stores[passthrough->layer_attribute]) {
        return false;
    }
    auto forwarded = passthrough->stores.mask;
    forwarded.reset(static_cast<size_t>(IR::Attribute::Layer));
    // SPIR-V initializes the vertex Position to (0, 0, 0, 1) in the prologue.
    // Fullscreen shaders often only store X/Y; the geometry stage copies default Z/W.
    // Those position components remain available even when absent from the store mask.
    for (u32 component = 0; component < 4; ++component) {
        forwarded.reset(static_cast<size_t>(IR::Attribute::PositionX + component));
    }
    if ((forwarded & ~vertex.info.stores.mask).any()) {
        return false;
    }
    bool found{};
    for (auto* block : vertex.blocks) {
        for (auto it = block->begin(); it != block->end(); ++it) {
            if (it->GetOpcode() == IR::Opcode::SetAttribute &&
                it->Arg(0).Attribute() == passthrough->layer_attribute) {
                IR::IREmitter ir{*block, std::next(it)};
                ir.SetAttribute(IR::Attribute::Layer, IR::F32{it->Arg(1)}, ir.Imm32(0u));
                found = true;
            }
        }
    }
    if (found) {
        // Retain exactly the geometry stage's varyings so absent fragment inputs still default.
        for (auto* block : vertex.blocks) {
            for (auto& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                    !passthrough->stores[inst.Arg(0).Attribute()]) {
                    inst.Invalidate();
                }
            }
        }
        vertex.info.stores = passthrough->stores;
    }
    return found;
}
} // namespace Shader::Optimization
