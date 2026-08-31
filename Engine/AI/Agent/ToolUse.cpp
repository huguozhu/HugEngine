#include "AI/Agent/ToolUse.h"

#include "nlohmann/json.hpp"

#include <vector>

using nlohmann::json;

namespace he::ai {

namespace {
// 工具清单（与 CompileAction 支持的 op 对齐）
const std::vector<ToolInfo> kTools = {
    {"SpawnEntity",  "新建实体。argsJson={name, transform{position,scale}, components[{type,Cube|Sphere|DirectionalLight|PointLight|PhysicalSky, ...}]}"},
    {"SetTransform", "修改实体 Transform。targetEntity=实体id，argsJson={position:[x,y,z], scale:[sx,sy,sz]}"},
    {"SetProperty",  "经反射修改组件属性。targetEntity=实体id，argsJson={component:\"he::TransformComponent\", property:\"position\", value:[x,y,z]}"},
};
} // namespace

const std::vector<ToolInfo>& ToolUse::GetTools() {
    return kTools;
}

String ToolUse::BuildToolSchema() {
    json arr = json::array();
    for (auto& t : kTools) {
        arr.push_back({{"name", t.name}, {"description", t.description}});
    }
    return arr.dump();
}

} // namespace he::ai
