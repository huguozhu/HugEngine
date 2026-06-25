// Samples/Editor/Panels/DetailsPanel.cpp

#include "DetailsPanel.h"
#include "Editor/EditorContext.h"
#include "Editor/Command.h"
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
    auto* sg = m_Ctx->GetSceneGraph();
    auto* cmdHistory = m_Ctx->GetCommandHistory();

    // 位置（实时预览 + 编辑完成后记录 Undo）
    float3 oldPos = t->position;
    float pos[3] = { oldPos.x, oldPos.y, oldPos.z };
    if (ImGui::DragFloat3("Position", pos, 0.1f)) {
        t->position = float3(pos[0], pos[1], pos[2]);
        if (sg) sg->MarkDirty(entity);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float3 newPos(pos[0], pos[1], pos[2]);
        if (glm::any(glm::notEqual(oldPos, newPos))) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Position",
                [t, sg, entity, oldPos]() { t->position = oldPos; if (sg) sg->MarkDirty(entity); },
                [t, sg, entity, newPos]() { t->position = newPos; if (sg) sg->MarkDirty(entity); }
            ));
        }
    }

    // 缩放
    float3 oldScl = t->scale;
    float scl[3] = { oldScl.x, oldScl.y, oldScl.z };
    if (ImGui::DragFloat3("Scale", scl, 0.1f, 0.01f, 100.0f)) {
        t->scale = float3(scl[0], scl[1], scl[2]);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float3 newScl(scl[0], scl[1], scl[2]);
        if (glm::any(glm::notEqual(oldScl, newScl))) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Scale",
                [t, oldScl]() { t->scale = oldScl; },
                [t, newScl]() { t->scale = newScl; }
            ));
        }
    }

    // 旋转（显示四元数分量，简化编辑）
    ImGui::Text("Rotation: (%.2f, %.2f, %.2f, %.2f)",
        t->rotation.w, t->rotation.x, t->rotation.y, t->rotation.z);
    // Phase 3-2: 改为欧拉角 DragFloat3 + Undo
}

void DetailsPanel::RenderMesh(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* mesh = world->GetComponent<MeshComponent>(entity);
    if (!mesh) return;
    auto* cmdHistory = m_Ctx->GetCommandHistory();

    // Base Color
    float4 oldColor = mesh->baseColorFactor;
    float color[4] = { oldColor.x, oldColor.y, oldColor.z, oldColor.w };
    if (ImGui::ColorEdit4("Base Color", color)) {
        mesh->baseColorFactor = float4(color[0], color[1], color[2], color[3]);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float4 newColor(color[0], color[1], color[2], color[3]);
        if (oldColor != newColor) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Base Color",
                [mesh, oldColor]() { mesh->baseColorFactor = oldColor; },
                [mesh, newColor]() { mesh->baseColorFactor = newColor; }
            ));
        }
    }

    // Metallic
    float oldMetallic = mesh->metallicFactor;
    if (ImGui::DragFloat("Metallic", &mesh->metallicFactor, 0.01f, 0.0f, 1.0f)) {
        // 值已通过指针直接修改
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newMetallic = mesh->metallicFactor;
        if (oldMetallic != newMetallic) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Metallic",
                [mesh, oldMetallic]() { mesh->metallicFactor = oldMetallic; },
                [mesh, newMetallic]() { mesh->metallicFactor = newMetallic; }
            ));
        }
    }

    // Roughness
    float oldRoughness = mesh->roughnessFactor;
    if (ImGui::DragFloat("Roughness", &mesh->roughnessFactor, 0.01f, 0.0f, 1.0f)) {
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newRoughness = mesh->roughnessFactor;
        if (oldRoughness != newRoughness) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Roughness",
                [mesh, oldRoughness]() { mesh->roughnessFactor = oldRoughness; },
                [mesh, newRoughness]() { mesh->roughnessFactor = newRoughness; }
            ));
        }
    }

    // Emissive
    float3 oldEmissive = mesh->emissiveFactor;
    float emissive[3] = { oldEmissive.x, oldEmissive.y, oldEmissive.z };
    if (ImGui::ColorEdit3("Emissive", emissive)) {
        mesh->emissiveFactor = float3(emissive[0], emissive[1], emissive[2]);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float3 newEmissive(emissive[0], emissive[1], emissive[2]);
        if (glm::any(glm::notEqual(oldEmissive, newEmissive))) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Emissive",
                [mesh, oldEmissive]() { mesh->emissiveFactor = oldEmissive; },
                [mesh, newEmissive]() { mesh->emissiveFactor = newEmissive; }
            ));
        }
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
    auto* cmdHistory = m_Ctx->GetCommandHistory();

    // 方向
    float3 oldDir = light->direction;
    float dir[3] = { oldDir.x, oldDir.y, oldDir.z };
    if (ImGui::DragFloat3("Direction", dir, 0.05f, -1.0f, 1.0f)) {
        light->direction = glm::normalize(float3(dir[0], dir[1], dir[2]));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float3 newDir = glm::normalize(float3(dir[0], dir[1], dir[2]));
        if (glm::any(glm::notEqual(oldDir, newDir))) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Light Direction",
                [light, oldDir]() { light->direction = oldDir; },
                [light, newDir]() { light->direction = newDir; }
            ));
        }
    }

    // 颜色
    float3 oldColor = light->color;
    float col[3] = { oldColor.x, oldColor.y, oldColor.z };
    if (ImGui::ColorEdit3("Color", col)) {
        light->color = float3(col[0], col[1], col[2]);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float3 newColor(col[0], col[1], col[2]);
        if (glm::any(glm::notEqual(oldColor, newColor))) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Light Color",
                [light, oldColor]() { light->color = oldColor; },
                [light, newColor]() { light->color = newColor; }
            ));
        }
    }

    // 强度
    float oldIntensity = light->intensity;
    if (ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 100.0f)) {
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newIntensity = light->intensity;
        if (oldIntensity != newIntensity) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Light Intensity",
                [light, oldIntensity]() { light->intensity = oldIntensity; },
                [light, newIntensity]() { light->intensity = newIntensity; }
            ));
        }
    }
}

} // namespace he::editor
