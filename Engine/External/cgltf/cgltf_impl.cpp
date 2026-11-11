// ============================================================
//  Engine/External/cgltf/cgltf_impl.cpp
//
//  cgltf 实现体（单头文件库的唯一展开点）。
//
//  【为什么单独一个 TU】此前 `CGLTF_IMPLEMENTATION` 只在
//  `Engine/Asset/Asset/glTFLoader.cpp` 里展开，于是任何"不链接 Asset"的使用方
//  （离线工具 `Tools/MeshPreprocess`、Nanite 的最小几何读取器）都没有符号可用；
//  而链接 Asset 又会把 ECS 依赖一起拖进来。抽成独立静态库后：
//    · HugEngineAsset（glTFLoader）与 HugEngineNaniteBuild（离线读取器）共用同一份实现；
//    · 全工程仍然只有一个 cgltf 实现，不会出现重复符号或两套解析行为。
// ============================================================

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
