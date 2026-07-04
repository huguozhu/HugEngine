// Samples/Editor/Panels/OutlinerPanel.cpp

#include "OutlinerPanel.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "imgui.h"

namespace he::editor {

void OutlinerPanel::Render() {
    if (!m_Ctx || !m_Ctx->GetWorld()) return;

    // 标题栏
    ImGui::Text("Outliner");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    char filterBuf[128] = {};
    if (!m_Filter.empty()) {
        std::strncpy(filterBuf, m_Filter.c_str(), sizeof(filterBuf) - 1);
    }
    if (ImGui::InputText("##Filter", filterBuf, sizeof(filterBuf))) {
        m_Filter = filterBuf;
    }
    ImGui::Separator();

    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();

    // Add Entity 按钮
    ImGui::SameLine(0, 4);
    if (ImGui::Button("+", ImVec2(24, 20))) ImGui::OpenPopup("AddEntityPopup");
    if (ImGui::BeginPopup("AddEntityPopup")) {
        auto createEntity = [&](const char* name) {
            Entity e = world->CreateEntity(name);
            world->AddComponent<TransformComponent>(e);
            if (sg) sg->SetParent(e, Entity{kInvalidEntity});
            m_Ctx->SelectEntity(e);
            return e;
        };
        if (ImGui::MenuItem("Empty Entity")) { createEntity("NewEntity"); }
        if (ImGui::MenuItem("Cube")) { Entity e = createEntity("Cube"); world->AddComponent<CubeComponent>(e); }
        if (ImGui::MenuItem("Sphere")) { Entity e = createEntity("Sphere"); world->AddComponent<SphereComponent>(e); }
        if (ImGui::MenuItem("Point Light")) {
            Entity e = createEntity("PointLight");
            auto* pl = world->AddComponent<PointLight>(e);
            pl->color = float3(1, 0.8f, 0.6f); pl->intensity = 20;
        }
        if (ImGui::MenuItem("Spot Light")) {
            Entity e = createEntity("SpotLight");
            auto* sl = world->AddComponent<SpotLight>(e);
            sl->color = float3(1, 0.9f, 0.7f); sl->intensity = 30;
        }
        if (ImGui::MenuItem("Directional Light")) {
            Entity e = createEntity("DirLight");
            auto* dl = world->AddComponent<DirectionalLight>(e);
            dl->color = float3(1, 0.95f, 0.85f); dl->intensity = 10;
        }
        ImGui::EndPopup();
    }

    // 一次性构建 parent→children 映射（O(N)），避免每节点递归时重复遍历全实体
    std::unordered_map<EntityID, TArray<Entity>> childrenMap;
    world->ForEachEntity([&](Entity e) {
        Entity parent = sg ? sg->GetParent(e) : Entity{kInvalidEntity};
        if (parent.IsValid()) {
            childrenMap[parent.id].push_back(e);
        }
    });

    // 遍历根实体（无父节点）
    world->ForEachEntity([&](Entity e) {
        Entity parent = sg ? sg->GetParent(e) : Entity{kInvalidEntity};
        if (!parent.IsValid()) {
            RenderEntity(e, 0, childrenMap);
        }
    });
}

void OutlinerPanel::RenderEntity(Entity entity, int depth,
                                  const std::unordered_map<EntityID, TArray<Entity>>& childrenMap) {
    auto* world = m_Ctx->GetWorld();
    if (!world) return;

    // 获取实体名称
    String name = "Entity #" + std::to_string(entity.id);

    // 搜索过滤（简单匹配检查）
    if (!m_Filter.empty()) {
        if (name.find(m_Filter) == String::npos) {
            return;
        }
    }

    // --- 实体类型图标 ---
    const char* icon = "  ";
    if (world->HasComponent<DirectionalLight>(entity) ||
        world->HasComponent<PointLight>(entity) ||
        world->HasComponent<SpotLight>(entity)) {
        icon = "(L)";  // Light
    } else if (world->HasComponent<MeshComponent>(entity) ||
               world->HasComponent<CubeComponent>(entity)) {
        icon = "(M)";  // Mesh
    }

    // 缩进
    String label = String(depth * 2, ' ') + icon + " " + name;

    // 选中高亮
    bool isSelected = m_Ctx->IsSelected(entity);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected)
        flags |= ImGuiTreeNodeFlags_Selected;

    // 从预计算映射 O(1) 判断是否有子实体
    auto it = childrenMap.find(entity.id);
    bool hasChildren = (it != childrenMap.end() && !it->second.empty());
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // 作为 TreeNode 节点渲染
    bool open = ImGui::TreeNodeEx((void*)(uintptr_t)entity.id, flags, "%s", label.c_str());

    // 点击选中
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_Ctx->SelectEntity(entity);
    }

    // 右键菜单
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete")) {
            world->DestroyEntity(entity);
            m_Ctx->DeselectAll();
        }
        if (ImGui::MenuItem("Rename")) {
            // TODO: 3-2 实现重命名弹窗
        }
        ImGui::EndPopup();
    }

    if (open) {
        // 从预计算映射 O(1) 获取子实体列表
        if (it != childrenMap.end()) {
            for (auto& child : it->second) {
                RenderEntity(child, depth + 1, childrenMap);
            }
        }
        ImGui::TreePop();
    }
}

} // namespace he::editor
