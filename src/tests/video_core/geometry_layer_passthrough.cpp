// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <iterator>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using namespace Shader;
using IR::Attribute;

struct Programs {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    IR::Program vertex;
    IR::Program geometry;

    Programs() {
        vertex.stage = Stage::VertexB;
        auto* block = AddBlock(vertex);
        IR::IREmitter ir{*block};
        for (u32 i = 0; i < 4; ++i) {
            ir.SetAttribute(Attribute::PositionX + i, ir.Imm32(0.0f), ir.Imm32(0u));
            vertex.info.stores.Set(Attribute::PositionX + i);
        }
        ir.SetAttribute(Attribute::Generic1X, ir.Imm32(0.0f), ir.Imm32(0u));
        vertex.info.stores.Set(Attribute::Generic1X, true);
        geometry.stage = Stage::Geometry;
        geometry.output_topology = OutputTopology::TriangleStrip;
        geometry.output_vertices = 3;
        geometry.invocations = 1;
    }

    IR::Block* AddBlock(IR::Program& program) {
        auto* block = block_pool.Create(inst_pool);
        program.blocks.push_back(block);
        IR::AbstractSyntaxNode node;
        node.type = IR::AbstractSyntaxNode::Type::Block;
        node.data.block = block;
        program.syntax_list.push_back(node);
        return block;
    }

    void EmitTriangle(IR::Block& block, u32 first = 0, u32 stream = 0,
                      bool change_position = false) {
        IR::IREmitter ir{block};
        for (u32 i = 0; i < 3; ++i) {
            const auto vertex_index = ir.Imm32((first + i) % 3);
            for (u32 c = 0; c < 4; ++c) {
                const auto attr = Attribute::PositionX + c;
                const auto source = change_position && c == 0 ? Attribute::PositionY : attr;
                ir.SetAttribute(attr, ir.GetAttribute(source, vertex_index), ir.Imm32(0u));
            }
            ir.SetAttribute(Attribute::Layer, ir.GetAttribute(Attribute::Generic1X, vertex_index),
                            ir.Imm32(0u));
            ir.EmitVertex(ir.Imm32(stream));
        }
        ir.EndPrimitive(ir.Imm32(stream));
        geometry.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
    }

    size_t LayerWrites() const {
        size_t count{};
        for (const auto* block : vertex.blocks) {
            for (const auto& inst : block->Instructions()) {
                count += inst.GetOpcode() == IR::Opcode::SetAttribute &&
                         inst.Arg(0).Attribute() == Attribute::Layer;
            }
        }
        return count;
    }
};
} // namespace

TEST_CASE("Layer-only geometry passthrough becomes vertex layer output", "[shader][geometry]") {
    Programs p;
    const auto original_layer_value = p.vertex.blocks.front()->back().Arg(1);
    p.EmitTriangle(*p.AddBlock(p.geometry));
    REQUIRE(Optimization::LowerGeometryLayerPassthrough(p.vertex, p.geometry));
    CHECK(p.LayerWrites() == 1);
    CHECK(p.vertex.info.stores[Attribute::Layer]);
    CHECK(p.vertex.info.stores.AllComponents(Attribute::PositionX));
    CHECK_FALSE(p.vertex.info.stores[Attribute::Generic1X]);
    const auto& last = p.vertex.blocks.front()->back();
    CHECK(original_layer_value == last.Arg(1));
    const auto& removed = *std::prev(p.vertex.blocks.front()->end(), 2);
    CHECK(removed.GetOpcode() == IR::Opcode::Void);
}

TEST_CASE("Geometry lowering rejects operations that alter primitives", "[shader][geometry]") {
    Programs p;
    auto* block = p.AddBlock(p.geometry);
    SECTION("reordered vertices") {
        p.EmitTriangle(*block, 1);
    }
    SECTION("another stream") {
        p.EmitTriangle(*block, 0, 1);
    }
    SECTION("changed position") {
        p.EmitTriangle(*block, 0, 0, true);
    }
    SECTION("memory side effect") {
        IR::IREmitter{*block}.DeviceMemoryBarrier();
        p.EmitTriangle(*block);
    }
    SECTION("multiple invocations") {
        p.geometry.invocations = 2;
        p.EmitTriangle(*block);
    }
    SECTION("missing vertex layer source") {
        p.vertex.info.stores.Set(Attribute::Generic1X, false);
        p.EmitTriangle(*block);
    }
    SECTION("existing vertex layer output") {
        p.vertex.info.stores.Set(Attribute::Layer, true);
        p.EmitTriangle(*block);
    }
    REQUIRE_FALSE(Optimization::LowerGeometryLayerPassthrough(p.vertex, p.geometry));
    CHECK(p.LayerWrites() == 0);
}

TEST_CASE("Fullscreen layer passthrough preserves default position components",
          "[shader][geometry]") {
    Programs p;
    p.vertex.info.stores.Set(Attribute::PositionZ, false);
    p.vertex.info.stores.Set(Attribute::PositionW, false);
    for (auto& inst : p.vertex.blocks.front()->Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
            (inst.Arg(0).Attribute() == Attribute::PositionZ ||
             inst.Arg(0).Attribute() == Attribute::PositionW)) {
            inst.Invalidate();
        }
    }
    p.EmitTriangle(*p.AddBlock(p.geometry));
    REQUIRE(Optimization::LowerGeometryLayerPassthrough(p.vertex, p.geometry));
    CHECK(p.LayerWrites() == 1);
    CHECK(p.vertex.info.stores.AllComponents(Attribute::PositionX));
}

TEST_CASE("Geometry proof follows constant branches and the actual phi predecessor",
          "[shader][geometry]") {
    Programs p;
    auto* entry = p.AddBlock(p.geometry);
    IR::IREmitter ir{*entry};
    const auto count = ir.BitFieldExtract(ir.InvocationInfo(), ir.Imm32(16u), ir.Imm32(8u));
    const auto cond = ir.ILessThanEqual(count, ir.Imm32(2u), false);
    auto* body = p.block_pool.Create(p.inst_pool);
    auto* merge = p.block_pool.Create(p.inst_pool);
    IR::AbstractSyntaxNode branch;
    branch.type = IR::AbstractSyntaxNode::Type::If;
    branch.data.if_node = {cond, body, merge};
    p.geometry.syntax_list.push_back(branch);
    IR::AbstractSyntaxNode body_node;
    body_node.type = IR::AbstractSyntaxNode::Type::Block;
    body_node.data.block = body;
    p.geometry.syntax_list.push_back(body_node);
    IR::IREmitter{*body}.DeviceMemoryBarrier();
    p.geometry.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::EndIf;
    IR::AbstractSyntaxNode merge_node;
    merge_node.type = IR::AbstractSyntaxNode::Type::Block;
    merge_node.data.block = merge;
    p.geometry.syntax_list.push_back(merge_node);
    merge->AppendNewInst(IR::Opcode::Phi, {});
    auto& phi = merge->back();
    phi.AddPhiOperand(entry, IR::Value{0u});
    phi.AddPhiOperand(body, IR::Value{2u});
    // This deliberately feeds a phi-selected index into the first vertex's position.
    p.EmitTriangle(*merge);
    for (auto& inst : merge->Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::GetAttribute) {
            inst.SetArg(1, IR::Value{&phi});
            break;
        }
    }
    REQUIRE(Optimization::LowerGeometryLayerPassthrough(p.vertex, p.geometry));
    CHECK(p.LayerWrites() == 1);
}
