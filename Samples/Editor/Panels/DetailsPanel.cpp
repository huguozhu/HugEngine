// Samples/Editor/Panels/DetailsPanel.cpp

#include "DetailsPanel.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "imgui.h"

namespace he::editor {

void DetailsPanel::Initialize(EditorContext* ctx) {
    m_Ctx = ctx;
}

void DetailsPanel::Render() {
    if (!m_Ctx) return;

    ImGui::Text("Details");
    ImGui::Separator();

    auto& selection = m_Ctx->GetSelection();
    if (selection.empty()) {
        ImGui::TextDisabled("No entity selected");
        return;
    }

    Entity entity = selection[0];
    auto* world = m_Ctx->GetWorld();
    if (!world || !world->IsValid(entity)) {
        ImGui::TextDisabled("Invalid entity");
        return;
    }

    // 实体名称 / ID
    ImGui::Text("Entity: #%llu", static_cast<unsigned long long>(entity.id));
    ImGui::Separator();

    // 按组件类型渲染属性
    if (world->HasComponent<TransformComponent>(entity))
        RenderTransform(world, entity);

    if (world->HasComponent<MeshComponent>(entity))
        RenderMesh(world, entity);

    if (world->HasComponent<DirectionalLight>(entity))
        RenderLight(world, entity);
}

void DetailsPanel::RenderTransform(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* t = world->GetComponent<TransformComponent>(entity);
    if (!t) return;

    // 位置
    float pos[3] = { t->position.x, t->position.y, t->position.z };
    if (ImGui::DragFloat3("Position", pos, 0.1f)) {
        t->position = float3(pos[0], pos[1], pos[2]);
        if (auto* sg = m_Ctx->GetSceneGraph())
            sg->MarkDirty(entity);
    }

    // 缩放
    float scl[3] = { t->scale.x, t->scale.y, t->scale.z };
    if (ImGui::DragFloat3("Scale", scl, 0.1f, 0.01f, 100.0f)) {
        t->scale = float3(scl[0], scl[1], scl[2]);
    }

    // 旋转（显示四元数分量，简化编辑）
    ImGui::Text("Rotation: (%.2f, %.2f, %.2f, %.2f)",
        t->rotation.w, t->rotation.x, t->rotation.y, t->rotation.z);
    // Phase 3-2: 改为欧拉角 DragFloat3
}

void DetailsPanel::RenderMesh(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* mesh = world->GetComponent<MeshComponent>(entity);
    if (!mesh) return;

    // Base Color
    float color[4] = {
        mesh->baseColorFactor.x,
        mesh->baseColorFactor.y,
        mesh->baseColorFactor.z,
        mesh->baseColorFactor.w
    };
    if (ImGui::ColorEdit4("Base Color", color)) {
        mesh->baseColorFactor = float4(color[0], color[1], color[2], color[3]);
    }

    // Metallic
    ImGui::DragFloat("Metallic", &mesh->metallicFactor, 0.01f, 0.0f, 1.0f);

    // Roughness
    ImGui::DragFloat("Roughness", &mesh->roughnessFactor, 0.01f, 0.0f, 1.0f);

    // Emissive (glTF 2.0)
    float emissive[3] = {
        mesh->emissiveFactor.x,
        mesh->emissiveFactor.y,
        mesh->emissiveFactor.z
    };
    if (ImGui::ColorEdit3("Emissive", emissive)) {
        mesh->emissiveFactor = float3(emissive[0], emissive[1], emissive[2]);
    }

    // 纹理路径显示（只读）
    if (!mesh->baseColorTexture.empty())
        ImGui::Text("BaseColor Tex: %s", mesh->baseColorTexture.c_str());
    if (!mesh->metallicRoughnessTexture.empty())
        ImGui::Text("MetallicRoughness Tex: %s", mesh->metallicRoughnessTexture.c_str());
    if (!mesh->normalTexture.empty())
        ImGui::Text("Normal Tex: %s", mesh->normalTexture.c_str());
    if (!mesh->emissiveTexture.empty())
        ImGui::Text("Emissive Tex: %s", mesh->emissiveTexture.c_str());
    if (!mesh->occlusionTexture.empty())
        ImGui::Text("Occlusion Tex: %s", mesh->occlusionTexture.c_str());
}

void DetailsPanel::RenderLight(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* light = world->GetComponent<DirectionalLight>(entity);
    if (!light) return;

    // 方向
    float dir[3] = { light->direction.x, light->direction.y, light->direction.z };
    if (ImGui::DragFloat3("Direction", dir, 0.05f, -1.0f, 1.0f)) {
        light->direction = glm::normalize(float3(dir[0], dir[1], dir[2]));
    }

    // 颜色
    float col[3] = { light->color.x, light->color.y, light->color.z };
    if (ImGui::ColorEdit3("Color", col)) {
        light->color = float3(col[0], col[1], col[2]);
    }

    // 强度
    ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 100.0f);
}

} // namespace he::editor
