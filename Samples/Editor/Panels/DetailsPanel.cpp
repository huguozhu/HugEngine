// Samples/Editor/Panels/DetailsPanel.cpp

#include "DetailsPanel.h"
#include "Editor/EditorContext.h"
#include "Editor/Command.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/CameraComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LevelComponent.h"
#include "imgui.h"
#include <glm/gtx/euler_angles.hpp>

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

    if (world->HasComponent<MeshComponent>(entity) ||
        world->HasComponent<CubeComponent>(entity) ||
        world->HasComponent<SphereComponent>(entity))
        RenderMesh(world, entity);

    // 检测光照组件类型（平行光 / 点光源 / 聚光灯 / 基础光源）
    if (world->HasComponent<DirectionalLight>(entity))
        RenderDirectionalLight(world, entity);
    else if (world->HasComponent<PointLight>(entity))
        RenderPointLight(world, entity);
    else if (world->HasComponent<SpotLight>(entity))
        RenderSpotLight(world, entity);
    else if (world->HasComponent<LightComponent>(entity))
        RenderLightBase(world, entity);

    // === Add Component 按钮 ===
    ImGui::Spacing(); ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0)))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup")) {
        static const char* items[] = {
            "Mesh", "Cube (Debug)", "Sphere (Debug)", "Level",
            "Point Light", "Spot Light", "Directional Light",
            "Camera", "Skybox"
        };
        for (int i = 0; i < IM_ARRAYSIZE(items); ++i) {
            if (ImGui::Selectable(items[i])) {
                if (strcmp(items[i], "Mesh") == 0) world->AddComponent<MeshComponent>(entity);
                else if (strcmp(items[i], "Level") == 0) world->AddComponent<LevelComponent>(entity);
                else if (strcmp(items[i], "Cube (Debug)") == 0) world->AddComponent<CubeComponent>(entity);
                else if (strcmp(items[i], "Sphere (Debug)") == 0) world->AddComponent<SphereComponent>(entity);
                else if (strcmp(items[i], "Point Light") == 0) { auto* l = world->AddComponent<PointLight>(entity); l->color=float3(1,0.8f,0.6f); l->intensity=20; }
                else if (strcmp(items[i], "Spot Light") == 0) { auto* l = world->AddComponent<SpotLight>(entity); l->color=float3(1,1,1); l->intensity=30; }
                else if (strcmp(items[i], "Directional Light") == 0) { auto* l = world->AddComponent<DirectionalLight>(entity); l->color=float3(1,0.95f,0.85f); l->intensity=10; }
                else if (strcmp(items[i], "Camera") == 0) world->AddComponent<CameraComponent>(entity);
                else if (strcmp(items[i], "Skybox") == 0) world->AddComponent<SkyboxComponent>(entity);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
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

    // 旋转（Euler 角度制显示）
    quat oldRot = t->rotation;
    float3 oldEuler = glm::degrees(glm::eulerAngles(oldRot));
    float euler[3] = { oldEuler.x, oldEuler.y, oldEuler.z };
    if (ImGui::DragFloat3("Rotation", euler, 0.5f, -360.0f, 360.0f, "%.1f°")) {
        quat newRot = glm::quat(glm::radians(float3(euler[0], euler[1], euler[2])));
        t->rotation = newRot;
        if (sg) sg->MarkDirty(entity);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        quat newRot = glm::quat(glm::radians(float3(euler[0], euler[1], euler[2])));
        if (glm::any(glm::notEqual(oldEuler, float3(euler[0], euler[1], euler[2])))) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Rotation",
                [t, sg, entity, oldRot]() { t->rotation = oldRot; if (sg) sg->MarkDirty(entity); },
                [t, sg, entity, newRot]() { t->rotation = newRot; if (sg) sg->MarkDirty(entity); }
            ));
        }
    }

    // 缩放（支持等比缩放）
    float3 oldScl = t->scale;
    float scl[3] = { oldScl.x, oldScl.y, oldScl.z };
    static bool uniformScale = false;
    ImGui::Checkbox("等比缩放", &uniformScale);
    ImGui::SameLine();
    if (ImGui::DragFloat3("Scale", scl, 0.1f, 0.01f, 100.0f)) {
        if (uniformScale) {
            // 找到改变的轴，同步到全部三轴
            if (scl[0] != oldScl.x) { scl[1] = scl[0]; scl[2] = scl[0]; }
            else if (scl[1] != oldScl.y) { scl[0] = scl[1]; scl[2] = scl[1]; }
            else if (scl[2] != oldScl.z) { scl[0] = scl[2]; scl[1] = scl[2]; }
        }
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
    if (!mesh) mesh = world->GetComponent<CubeComponent>(entity);
    if (!mesh) mesh = world->GetComponent<SphereComponent>(entity);
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

    // Alpha Mode（Opaque / Mask / Blend）
    u8 oldAlphaMode = mesh->alphaMode;
    const char* alphaItems[] = {"Opaque", "Mask", "Blend"};
    int alphaIdx = (int)mesh->alphaMode;
    if (ImGui::Combo("Alpha Mode", &alphaIdx, alphaItems, 3)) {
        mesh->alphaMode = (u8)alphaIdx;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && oldAlphaMode != mesh->alphaMode) {
        cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
            "Change Alpha Mode",
            [mesh, oldAlphaMode]() { mesh->alphaMode = oldAlphaMode; },
            [mesh, a = mesh->alphaMode]() { mesh->alphaMode = a; }
        ));
    }

    // Alpha Cutoff（仅 Mask 模式可用）
    if (mesh->alphaMode == 1) {
        float oldCutoff = mesh->alphaCutoff;
        if (ImGui::DragFloat("Alpha Cutoff", &mesh->alphaCutoff, 0.01f, 0.0f, 1.0f)) {}
        if (ImGui::IsItemDeactivatedAfterEdit() && oldCutoff != mesh->alphaCutoff) {
            float newCutoff = mesh->alphaCutoff;
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Alpha Cutoff",
                [mesh, oldCutoff]() { mesh->alphaCutoff = oldCutoff; },
                [mesh, newCutoff]() { mesh->alphaCutoff = newCutoff; }
            ));
        }
    }

    // Double Sided
    bool oldDS = mesh->doubleSided;
    if (ImGui::Checkbox("Double Sided", &mesh->doubleSided)) {}
    if (ImGui::IsItemDeactivatedAfterEdit() && oldDS != mesh->doubleSided) {
        cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
            "Change Double Sided",
            [mesh, oldDS]() { mesh->doubleSided = oldDS; },
            [mesh, d = mesh->doubleSided]() { mesh->doubleSided = d; }
        ));
    }

    // Unlit
    bool oldUnlit = mesh->unlit;
    if (ImGui::Checkbox("Unlit", &mesh->unlit)) {}
    if (ImGui::IsItemDeactivatedAfterEdit() && oldUnlit != mesh->unlit) {
        cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
            "Change Unlit",
            [mesh, oldUnlit]() { mesh->unlit = oldUnlit; },
            [mesh, u = mesh->unlit]() { mesh->unlit = u; }
        ));
    }

    // AO Strength
    float oldAO = mesh->aoFactor;
    if (ImGui::DragFloat("AO Strength", &mesh->aoFactor, 0.01f, 0.0f, 1.0f)) {}
    if (ImGui::IsItemDeactivatedAfterEdit() && oldAO != mesh->aoFactor) {
        float newAO = mesh->aoFactor;
        cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
            "Change AO Strength",
            [mesh, oldAO]() { mesh->aoFactor = oldAO; },
            [mesh, newAO]() { mesh->aoFactor = newAO; }
        ));
    }
}

void DetailsPanel::RenderLightBase(World* world, Entity entity) {
    auto* light = world->GetComponent<LightComponent>(entity);
    if (!light) return;
    auto* cmdHistory = m_Ctx->GetCommandHistory();

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

    // 投射阴影
    bool oldShadow = light->castShadow;
    if (ImGui::Checkbox("Cast Shadow", &light->castShadow)) {
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && oldShadow != light->castShadow) {
        cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
            "Change Cast Shadow",
            [light, oldShadow]() { light->castShadow = oldShadow; },
            [light, s = light->castShadow]() { light->castShadow = s; }
        ));
    }
}

void DetailsPanel::RenderDirectionalLight(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* light = world->GetComponent<DirectionalLight>(entity);
    if (!light) return;
    auto* cmdHistory = m_Ctx->GetCommandHistory();

    // 光源公共属性
    RenderLightBase(world, entity);

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
}

void DetailsPanel::RenderPointLight(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* light = world->GetComponent<PointLight>(entity);
    if (!light) return;
    auto* cmdHistory = m_Ctx->GetCommandHistory();

    // 光源公共属性
    RenderLightBase(world, entity);

    // 范围
    float oldRange = light->range;
    if (ImGui::DragFloat("Range", &light->range, 0.1f, 0.0f, 1000.0f)) {
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newRange = light->range;
        if (oldRange != newRange) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Light Range",
                [light, oldRange]() { light->range = oldRange; },
                [light, newRange]() { light->range = newRange; }
            ));
        }
    }
}

void DetailsPanel::RenderSpotLight(World* world, Entity entity) {
    if (!ImGui::CollapsingHeader("Spot Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    auto* light = world->GetComponent<SpotLight>(entity);
    if (!light) return;
    auto* cmdHistory = m_Ctx->GetCommandHistory();

    // 光源公共属性
    RenderLightBase(world, entity);

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

    // 范围
    float oldRange = light->range;
    if (ImGui::DragFloat("Range", &light->range, 0.1f, 0.0f, 1000.0f)) {
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newRange = light->range;
        if (oldRange != newRange) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Light Range",
                [light, oldRange]() { light->range = oldRange; },
                [light, newRange]() { light->range = newRange; }
            ));
        }
    }

    // 内锥角 / 外锥角（UI 显示为角度，内部存储为弧度）
    constexpr float RAD2DEG = 180.0f / 3.14159265f;
    constexpr float DEG2RAD = 3.14159265f / 180.0f;

    float oldInner = light->innerConeAngle;
    float innerDeg = oldInner * RAD2DEG;
    if (ImGui::SliderFloat("Inner Cone Angle", &innerDeg, 0.0f, 180.0f, "%.1f deg")) {
        light->innerConeAngle = innerDeg * DEG2RAD;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newInner = light->innerConeAngle;
        if (oldInner != newInner) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Inner Cone Angle",
                [light, oldInner]() { light->innerConeAngle = oldInner; },
                [light, newInner]() { light->innerConeAngle = newInner; }
            ));
        }
    }

    float oldOuter = light->outerConeAngle;
    float outerDeg = oldOuter * RAD2DEG;
    if (ImGui::SliderFloat("Outer Cone Angle", &outerDeg, 0.0f, 180.0f, "%.1f deg")) {
        light->outerConeAngle = outerDeg * DEG2RAD;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float newOuter = light->outerConeAngle;
        if (oldOuter != newOuter) {
            cmdHistory->Execute(std::make_unique<PropertyChangeCommand>(
                "Change Outer Cone Angle",
                [light, oldOuter]() { light->outerConeAngle = oldOuter; },
                [light, newOuter]() { light->outerConeAngle = newOuter; }
            ));
        }
    }
}

} // namespace he::editor
