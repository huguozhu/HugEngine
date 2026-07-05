// Panels/LevelLoader.cpp — Level 加载/卸载实现
#include "LevelLoader.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/LevelComponent.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/CameraComponent.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/AnimationComponent.h"
#include "Serialize/BinaryArchive.h"
#include "Reflect/ReflectionAPI.h"
#include "Core/Log.h"

#include <fstream>
#include <unordered_map>
#include <vector>

namespace he::editor {

// 属性反序列化（与 SceneSerializer 共用逻辑）
namespace {
void DeserProp(serialize::IArchive& ar, StringView tn,
               const reflect::PropertyInfo& prop, void* ptr) {
    if      (tn == "bool")   { ar.Serialize(prop.name, *(bool*)ptr); }
    else if (tn == "i32")    { ar.Serialize(prop.name, *(i32*)ptr); }
    else if (tn == "u32")    { ar.Serialize(prop.name, *(u32*)ptr); }
    else if (tn == "i64")    { ar.Serialize(prop.name, *(i64*)ptr); }
    else if (tn == "u64")    { ar.Serialize(prop.name, *(u64*)ptr); }
    else if (tn == "f32")    { ar.Serialize(prop.name, *(f32*)ptr); }
    else if (tn == "f64")    { ar.Serialize(prop.name, *(f64*)ptr); }
    else if (tn == "String") { ar.Serialize(prop.name, *(String*)ptr); }
    else if (tn == "float3") { ar.Serialize(prop.name, *(float3*)ptr); }
    else if (tn == "float4") { ar.Serialize(prop.name, *(float4*)ptr); }
    else if (tn == "quat")   { ar.Serialize(prop.name, *(quat*)ptr); }
}

Component* AddComponentByHash(World* world, Entity ent, u64 hash) {
    auto* cls = reflect::TypeRegistry::Instance().FindClassByHash(hash);
    if (!cls) return nullptr;
    StringView cn = cls->name;
    if      (cn == "MeshComponent")       return world->AddComponent<MeshComponent>(ent);
    else if (cn == "CubeComponent")       return world->AddComponent<CubeComponent>(ent);
    else if (cn == "SphereComponent")     return world->AddComponent<SphereComponent>(ent);
    else if (cn == "DirectionalLight")    return world->AddComponent<DirectionalLight>(ent);
    else if (cn == "PointLight")          return world->AddComponent<PointLight>(ent);
    else if (cn == "SpotLight")           return world->AddComponent<SpotLight>(ent);
    else if (cn == "LightComponent")      return world->AddComponent<LightComponent>(ent);
    else if (cn == "SkyboxComponent")     return world->AddComponent<SkyboxComponent>(ent);
    else if (cn == "CameraComponent")     return world->AddComponent<CameraComponent>(ent);
    else if (cn == "AnimationComponent")  return world->AddComponent<AnimationComponent>(ent);
    return nullptr;
}
} // namespace

void LevelLoader::LoadLevel(World& world, LevelComponent& lc) {
    if (lc.levelPath.empty() || lc.IsExpanded()) return;

    std::ifstream f(lc.levelPath.c_str(), std::ios::binary | std::ios::ate);
    if (!f) { HE_CORE_WARN("LevelLoader: {} not found", lc.levelPath); return; }

    std::vector<u8> buf((usize)f.tellg());
    f.seekg(0); f.read((char*)buf.data(), buf.size());
    usize p = 0;
    auto r32=[&](){u32 v=0;for(int i=0;i<4;++i)v|=(u32)buf[p++]<<(i*8);return v;};
    auto r64=[&](){u64 v=0;for(int i=0;i<8;++i)v|=(u64)buf[p++]<<(i*8);return v;};

    if (r32() != 0x43534548) { HE_CORE_ERROR("LevelLoader: bad magic"); return; }
    u32 ver = r32();
    u32 ec = r32();

    HE_CORE_INFO("LevelLoader: {} → {} entities", lc.levelPath, ec);

    auto* sg = world.GetSceneGraph();
    Entity parent = lc.GetEntity();
    std::unordered_map<u64, Entity> idMap;
    TArray<Entity> children;

    for (u32 ei = 0; ei < ec; ++ei) {
        u64 eid = r64();
        Entity ent = world.CreateEntity("LEnt");
        world.AddComponent<TransformComponent>(ent);
        idMap[eid] = ent;
        children.push_back(ent);

        u32 cc = r32();
        for (u32 ci = 0; ci < cc; ++ci) {
            u64 hash = r64(); u32 ds = r32();
            Component* added = AddComponentByHash(&world, ent, hash);
            if (!added) { p += ds; continue; }

            serialize::BinaryArchive ar(serialize::ArchiveMode::Read);
            ar.SetBuffer(std::vector<u8>(buf.begin()+p, buf.begin()+p+ds));
            ar.BeginObject("comp");
            reflect::ForEachProperty<Component>(added, [&](const reflect::PropertyInfo& pr, void* ptr) {
                if (pr.flags & reflect::PF_Serializable) DeserProp(ar, pr.typeName, pr, ptr);
            });
            ar.EndObject();
            p += ds;
        }
    }

    // 层级
    u32 pc = r32();
    for (u32 pi = 0; pi < pc; ++pi) {
        u64 c = r64(), par = r64();
        if (sg) { auto itC=idMap.find(c), itP=idMap.find(par);
            if (itC!=idMap.end() && itP!=idMap.end()) sg->SetParent(itC->second, itP->second); }
    }

    // 将没有保存 parent 的根实体挂到 LevelComponent 下
    for (auto& ent : children) {
        Entity ph = sg ? sg->GetParent(ent) : Entity{kInvalidEntity};
        if (!ph.IsValid() || idMap.find(ph.id) == idMap.end())
            if (sg) sg->SetParent(ent, parent);
    }

    lc.SetExpanded(true);
    lc.GetChildren() = std::move(children);
}

void LevelLoader::UnloadLevel(World& world, LevelComponent& lc) {
    if (!lc.IsExpanded()) return;
    for (auto& ent : lc.GetChildren())
        if (ent.IsValid()) world.DestroyEntity(ent);
    lc.GetChildren().clear();
    lc.SetExpanded(false);
}

void LevelLoader::SyncAll(World& world) {
    world.ForEach<LevelComponent>([&](Entity, LevelComponent& lc) {
        if (!lc.IsExpanded() && !lc.levelPath.empty())
            LoadLevel(world, lc);
    });
}

} // namespace he::editor
