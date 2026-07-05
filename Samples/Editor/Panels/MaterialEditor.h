// Panels/MaterialEditor.h — 可视化材质编辑器（节点图）
#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace he::editor {

// --- 引脚类型 ---
enum class PinKind : u8 { Float, Float2, Float3, Float4, Color, Texture };
inline const char* PinKindName(PinKind k) {
    switch(k){case PinKind::Float:return"Float";case PinKind::Float2:return"Float2";
    case PinKind::Float3:return"Float3";case PinKind::Float4:return"Float4";
    case PinKind::Color:return"Color";case PinKind::Texture:return"Texture";}return"?";}

// --- 引脚 ---
struct MPin {
    u64 id; std::string label; PinKind kind; bool isInput; u64 nodeId;
};

// --- 连线 ---
struct MLink { u64 id; u64 fromPin; u64 toPin; };

// --- 节点 ---
struct MNode {
    u64 id; std::string name; float2 pos{0,0};
    std::vector<MPin> inputs, outputs;
    // 节点数据
    float  floatVal = 0.0f;
    float4 colorVal = float4(1);
    std::string texPath;
};

class MaterialEditor {
public:
    bool m_Visible = false;
    void Render();

private:
    void DrawNode(const MNode& n);
    void DrawLink(const MLink& l);
    float2 PinPos(const MPin& p);
    void AddNode(const std::string& type, const float2& pos);

    std::vector<MNode> m_Nodes;
    std::vector<MLink> m_Links;
    u64 m_NextId = 1;

    // 交互状态
    u64  m_DragNode = 0;
    u64  m_DragPin  = 0;
    u64  m_SelectPin = 0;
    float2 m_CanvasOffset{0,0};
    float2 m_DragStart{0,0};
    float2 m_NodeDragStart{0,0};
    float2 m_LinkTempEnd{0,0};
};

} // namespace he::editor
