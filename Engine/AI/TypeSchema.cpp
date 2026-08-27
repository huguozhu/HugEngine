#include "AI/TypeSchema.h"

namespace he::ai {

String BuildTypeSchema() {
    // 只列出 MVP 支持的组件类型与字段；LLM 只能引用这里出现的类型
    return R"({
  "component_types": {
    "Cube":             {"fields": ["halfExtent", "baseColor", "metallic", "roughness"]},
    "Sphere":           {"fields": ["radius", "segmentCount", "ringCount", "baseColor", "metallic", "roughness"]},
    "DirectionalLight": {"fields": ["direction", "color", "intensity", "castShadow"]},
    "PointLight":       {"fields": ["color", "intensity", "range"]},
    "PhysicalSky":      {"fields": ["sunDirection", "turbidity", "intensity"]}
  },
  "transform": {"fields": ["position [x,y,z] 米", "scale [x,y,z] 可选"]},
  "color_format": "[r,g,b] 0~1",
  "notes": "每个实体必须有 transform.position；场景至少一个光源和一个地面"
})";
}

} // namespace he::ai
