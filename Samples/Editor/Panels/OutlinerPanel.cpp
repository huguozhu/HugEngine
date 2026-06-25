// Samples/Editor/Panels/OutlinerPanel.cpp

#include "OutlinerPanel.h"
#include "Editor/EditorContext.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/LightComponent.h"
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

    // 遍历所有根实体（无父节点的实体视为顶层）
    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();

    world->ForEachEntity([&](Entity e) {
        Entity parent = sg ? sg->GetParent(e) : Entity{kInvalidEntity};
        if (!parent.IsValid()) {
            // 根实体
            RenderEntity(e, 0);
        }
    });
}

void OutlinerPanel::RenderEntity(Entity entity, int depth) {
    auto* world = m_Ctx->GetWorld();
    auto* sg    = m_Ctx->GetSceneGraph();
    if (!world || !sg) return;

    // 获取实体名称
    auto* t = world->GetComponent<TransformComponent>(entity);
    String name = "Entity#" + std::to_string(entity.id);
    if (t) {
        // 尝试从组件获取名称（后续可扩展为 NameComponent）
        name = "Entity #" + std::to_string(entity.id);
    }

    // 搜索过滤（简单匹配检查）
    if (!m_Filter.empty()) {
        if (name.find(m_Filter) == String::npos) {
            // 不匹配则跳过该节点（完整实现需递归检查子树）
            // 简化处理：不匹配不渲染该节点
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

    // 先检查是否有子实体（决定是否显示展开箭头）
    bool hasChildren = false;
    world->ForEachEntity([&](Entity child) {
        if (!hasChildren) {
            Entity parent = sg->GetParent(child);
            if (parent == entity) {
                hasChildren = true;
            }
        }
    });
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
            // Phase 3-1: 简单设为 DestroyEntity
            world->DestroyEntity(entity);
            m_Ctx->DeselectAll();
        }
        if (ImGui::MenuItem("Rename")) {
            // TODO: 3-2 实现重命名弹窗
        }
        ImGui::EndPopup();
    }

    if (open) {
        // 渲染子实体（遍历所有实体，找 parent == entity 的子节点）
        world->ForEachEntity([&](Entity child) {
            Entity parent = sg->GetParent(child);
            if (parent == entity) {
                RenderEntity(child, depth + 1);
            }
        });
        ImGui::TreePop();
    }
}

} // namespace he::editor
