// Panels/MaterialEditor.cpp — 材质编辑器实现
#include "MaterialEditor.h"
#include "imgui.h"
#include <cmath>

namespace he::editor {

static constexpr float kPinRadius = 5.0f;
static constexpr float kNodeWidth  = 160.0f;
static constexpr float kPinHeight  = 18.0f;
static constexpr u32 kColorBg    = IM_COL32(40,40,48,255);
static constexpr u32 kColorPin   = IM_COL32(120,180,255,255);
static constexpr u32 kColorLink  = IM_COL32(200,200,200,255);
static constexpr u32 kColorFloat = IM_COL32(255,180,80,255);
static constexpr u32 kColorTex   = IM_COL32(80,200,255,255);
static constexpr u32 kColorOut   = IM_COL32(255,220,100,255);

float2 MaterialEditor::PinPos(const MPin& p) {
    // 找对应节点
    for (auto& n : m_Nodes) {
        if (n.id != p.nodeId) continue;
        float h = 28.0f + (p.isInput ? (float)n.inputs.size() * kPinHeight : 0);
        // 在当前节点的 pin 列表中找到这个 pin 的索引
        auto& pins = p.isInput ? n.inputs : n.outputs;
        int idx = 0;
        for (int i=0;i<(int)pins.size();++i){if(pins[i].id==p.id){idx=i;break;}}
        float y = n.pos.y + 28.0f + kPinHeight * 0.5f + idx * kPinHeight;
        float x = n.pos.x + (p.isInput ? kPinRadius : kNodeWidth - kPinRadius);
        if (!p.isInput) {
            // outputs are below inputs
            y = n.pos.y + 28.0f + std::max((float)n.inputs.size(),0.0f)*kPinHeight + kPinHeight*0.5f + idx*kPinHeight;
        }
        return float2(x, y);
    }
    return float2(0,0);
}

void MaterialEditor::DrawNode(const MNode& n) {
    auto* dl = ImGui::GetWindowDrawList();
    float h = 28.0f + n.inputs.size() * kPinHeight + n.outputs.size() * kPinHeight + 8.0f;

    float2 p = n.pos + m_CanvasOffset;
    dl->AddRectFilled(ImVec2(p.x,p.y), ImVec2(p.x+kNodeWidth,p.y+h), kColorBg, 4.0f);
    dl->AddRect(ImVec2(p.x,p.y), ImVec2(p.x+kNodeWidth,p.y+h), IM_COL32(100,100,120,255), 4.0f);
    dl->AddText(ImVec2(p.x+8,p.y+5), IM_COL32(255,255,255,255), n.name.c_str());

    float y = p.y + 28.0f;
    for (auto& pi : n.inputs) {
        float2 pp(p.x, y + kPinHeight*0.5f);
        u32 col = kColorPin;
        if (pi.kind == PinKind::Float || pi.kind == PinKind::Float3) col = kColorFloat;
        if (pi.kind == PinKind::Texture) col = kColorTex;
        dl->AddCircleFilled(ImVec2(pp.x, pp.y), kPinRadius, col);
        dl->AddText(ImVec2(pp.x+10, pp.y-7), IM_COL32(200,200,200,255), pi.label.c_str());
        y += kPinHeight;
    }
    for (auto& po : n.outputs) {
        float2 pp(p.x + kNodeWidth, y + kPinHeight*0.5f);
        dl->AddCircleFilled(ImVec2(pp.x, pp.y), kPinRadius, kColorOut);
        dl->AddText(ImVec2(pp.x - 10 - ImGui::CalcTextSize(po.label.c_str()).x, pp.y-7), IM_COL32(200,200,200,255), po.label.c_str());
        y += kPinHeight;
    }

    // 节点内部数据编辑
    if (n.name == "Float") {
        ImGui::SetCursorScreenPos(ImVec2(p.x+50, p.y+30));
        ImGui::PushID((int)n.id);
        float v = n.floatVal;
        ImGui::SetNextItemWidth(100);
        if (ImGui::DragFloat("##fv",&v,0.01f)) const_cast<MNode&>(n).floatVal=v;
        ImGui::PopID();
    }
    if (n.name == "Color") {
        ImGui::SetCursorScreenPos(ImVec2(p.x+30, p.y+30));
        ImGui::PushID((int)n.id);
        float c[4]={n.colorVal.x,n.colorVal.y,n.colorVal.z,n.colorVal.w};
        ImGui::SetNextItemWidth(120);
        if (ImGui::ColorEdit4("##cv",c,ImGuiColorEditFlags_NoInputs)) const_cast<MNode&>(n).colorVal=float4(c[0],c[1],c[2],c[3]);
        ImGui::PopID();
    }
}

void MaterialEditor::DrawLink(const MLink& l) {
    auto* dl = ImGui::GetWindowDrawList();
    MPin *from=nullptr, *to=nullptr;
    for (auto& n : m_Nodes) {
        for (auto& p : n.inputs) if (p.id == l.toPin) to = &p;
        for (auto& p : n.outputs) if (p.id == l.fromPin) from = &p;
    }
    if (!from || !to) return;
    float2 p1 = PinPos(*from) + m_CanvasOffset;
    float2 p2 = PinPos(*to) + m_CanvasOffset;
    float2 cp1(p1.x+50, p1.y), cp2(p2.x-50, p2.y);
    dl->AddBezierCubic(ImVec2(p1.x,p1.y),ImVec2(cp1.x,cp1.y),ImVec2(cp2.x,cp2.y),ImVec2(p2.x,p2.y),kColorLink,2.0f);
}

void MaterialEditor::AddNode(const std::string& type, const float2& pos) {
    MNode n; n.id=m_NextId++; n.name=type; n.pos=pos - m_CanvasOffset;
    if(type=="PBR Output"){n.inputs.push_back({m_NextId++,"BaseColor",PinKind::Color,true,n.id});n.inputs.push_back({m_NextId++,"Metallic",PinKind::Float,true,n.id});n.inputs.push_back({m_NextId++,"Roughness",PinKind::Float,true,n.id});}
    else if(type=="Texture2D"){n.outputs.push_back({m_NextId++,"Color",PinKind::Color,false,n.id});n.outputs.push_back({m_NextId++,"Alpha",PinKind::Float,false,n.id});}
    else if(type=="Float"){n.outputs.push_back({m_NextId++,"Value",PinKind::Float,false,n.id});}
    else if(type=="Color"){n.outputs.push_back({m_NextId++,"Color",PinKind::Color,false,n.id});}
    else if(type=="Multiply"){n.inputs.push_back({m_NextId++,"A",PinKind::Float,true,n.id});n.inputs.push_back({m_NextId++,"B",PinKind::Float,true,n.id});n.outputs.push_back({m_NextId++,"Result",PinKind::Float,false,n.id});}
    else if(type=="Lerp"){n.inputs.push_back({m_NextId++,"A",PinKind::Float3,true,n.id});n.inputs.push_back({m_NextId++,"B",PinKind::Float3,true,n.id});n.inputs.push_back({m_NextId++,"T",PinKind::Float,true,n.id});n.outputs.push_back({m_NextId++,"Result",PinKind::Float3,false,n.id});}
    m_Nodes.push_back(n);
}

void MaterialEditor::Render() {
    if (!m_Visible) return;
    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("Material Editor", &m_Visible);

    // 默认节点
    if (m_Nodes.empty()) { AddNode("PBR Output", float2(300, 200)); AddNode("Float", float2(100, 200)); }

    // 工具栏
    if (ImGui::Button("+ PBR Output")) AddNode("PBR Output", float2(300, 100));
    ImGui::SameLine(); if (ImGui::Button("+ Float")) AddNode("Float", float2(100, 100));
    ImGui::SameLine(); if (ImGui::Button("+ Color")) AddNode("Color", float2(100, 250));
    ImGui::SameLine(); if (ImGui::Button("+ Texture2D")) AddNode("Texture2D", float2(100, 400));
    ImGui::SameLine(); if (ImGui::Button("+ Multiply")) AddNode("Multiply", float2(100, 100));
    ImGui::SameLine(); if (ImGui::Button("+ Lerp")) AddNode("Lerp", float2(100, 100));
    ImGui::Separator();

    // 画布
    ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvasMin, ImVec2(canvasMin.x+canvasSize.x,canvasMin.y+canvasSize.y), IM_COL32(30,30,35,255));
    ImGui::InvisibleButton("##Canvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);

    // --- 交互 ---
    ImVec2 mPos = ImGui::GetMousePos();
    float2 mp(mPos.x, mPos.y);
    bool canvasHovered = ImGui::IsItemHovered();
    float2 canvasPos(canvasMin.x, canvasMin.y);

    // 右键菜单
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("NodeMenu");
    if (ImGui::BeginPopup("NodeMenu")) {
        if (ImGui::MenuItem("Float")) AddNode("Float", mp - canvasPos);
        if (ImGui::MenuItem("Color"))  AddNode("Color", mp - canvasPos);
        if (ImGui::MenuItem("Texture2D")) AddNode("Texture2D", mp - canvasPos);
        if (ImGui::MenuItem("Multiply"))  AddNode("Multiply", mp - canvasPos);
        if (ImGui::MenuItem("Lerp")) AddNode("Lerp", mp - canvasPos);
        ImGui::EndPopup();
    }

    // 拖拽节点
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canvasHovered) {
        for (auto& n : m_Nodes) {
            float2 np = n.pos + m_CanvasOffset;
            float h = 28.0f + n.inputs.size()*kPinHeight + n.outputs.size()*kPinHeight + 8.0f;
            if (mp.x>=np.x && mp.x<=np.x+kNodeWidth && mp.y>=np.y && mp.y<=np.y+h) {
                // 检查是否点击了 pin
                bool hitPin = false;
                for (auto& pi : n.inputs) {
                    float2 pp = PinPos(pi) + m_CanvasOffset;
                    if (glm::length(mp - pp) < kPinRadius+3) { m_DragPin = pi.id; hitPin = true; break; }
                }
                for (auto& po : n.outputs) {
                    if (hitPin) break;
                    float2 pp = PinPos(po) + m_CanvasOffset;
                    if (glm::length(mp - pp) < kPinRadius+3) { m_DragPin = po.id; hitPin = true; break; }
                }
                if (!hitPin) { m_DragNode = n.id; m_NodeDragStart = n.pos - mp; }
                break;
            }
        }
    }

    // 画布拖拽
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !m_DragNode && !m_DragPin)
        m_CanvasOffset += float2(ImGui::GetMouseDragDelta().x, ImGui::GetMouseDragDelta().y);

    // 移动节点
    if (m_DragNode && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        for (auto& n : m_Nodes) if (n.id == m_DragNode) n.pos = mp + m_NodeDragStart;
    }

    // 拖拽连线
    if (m_DragPin && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        m_LinkTempEnd = mp;

    // 释放连线
    if (m_DragPin && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        // 查找目标 pin
        for (auto& n : m_Nodes) {
            for (auto& pi : n.inputs) {
                float2 pp = PinPos(pi) + m_CanvasOffset;
                if (glm::length(mp - pp) < kPinRadius+3 && pi.id != m_DragPin) {
                    // 创建连线: output->input
                    bool srcIsOut = false, dstIsIn = false;
                    for (auto& n2 : m_Nodes) { for (auto& po : n2.outputs) if (po.id == m_DragPin) srcIsOut = true; }
                    dstIsIn = pi.isInput;
                    if (srcIsOut && dstIsIn)
                        m_Links.push_back({m_NextId++, m_DragPin, pi.id});
                    break;
                }
            }
        }
        m_DragPin = 0;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { m_DragNode = 0; m_DragPin = 0; ImGui::ResetMouseDragDelta(); }

    // --- 绘制 ---
    // Grid
    for (float x = fmodf(m_CanvasOffset.x, 40); x < canvasSize.x; x += 40)
        dl->AddLine(ImVec2(canvasMin.x+x,canvasMin.y), ImVec2(canvasMin.x+x,canvasMin.y+canvasSize.y), IM_COL32(50,50,55,255));
    for (float y = fmodf(m_CanvasOffset.y, 40); y < canvasSize.y; y += 40)
        dl->AddLine(ImVec2(canvasMin.x,canvasMin.y+y), ImVec2(canvasMin.x+canvasSize.x,canvasMin.y+y), IM_COL32(50,50,55,255));

    // Links
    for (auto& l : m_Links) DrawLink(l);
    // 拖拽中的临时线
    if (m_DragPin) {
        MPin* p = nullptr; for (auto& n : m_Nodes) { for (auto& pi : n.inputs) if (pi.id == m_DragPin) p = &pi; for (auto& po : n.outputs) if (po.id == m_DragPin) p = &po; }
        if (p) { float2 pp = PinPos(*p)+m_CanvasOffset; dl->AddBezierCubic(ImVec2(pp.x,pp.y),ImVec2(pp.x+50,pp.y),ImVec2(m_LinkTempEnd.x-50,m_LinkTempEnd.y),ImVec2(m_LinkTempEnd.x,m_LinkTempEnd.y),kColorLink,2.0f); }
    }

    // Nodes
    for (auto& n : m_Nodes) DrawNode(n);

    ImGui::End();
}

} // namespace he::editor
