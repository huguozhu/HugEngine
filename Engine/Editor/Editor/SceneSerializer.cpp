// Engine/Editor/Private/SceneSerializer.cpp

#include "Editor/SceneSerializer.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Serialize/BinaryArchive.h"
#include "Reflect/ReflectionAPI.h"
#include "Core/Log.h"

#include <fstream>
#include <unordered_map>
#include <vector>

namespace he::editor {

// ----------------------------------------------------------
// 属性类型分派序列化（Save 和 Load 共用）
// ----------------------------------------------------------
static void SerializeProperty(serialize::IArchive& ar, StringView typeName,
                               const reflect::PropertyInfo& prop, void* ptr) {
    if      (typeName == "bool")   { ar.Serialize(prop.name, *(bool*)ptr); }
    else if (typeName == "i32")    { ar.Serialize(prop.name, *(i32*)ptr); }
    else if (typeName == "u32")    { ar.Serialize(prop.name, *(u32*)ptr); }
    else if (typeName == "i64")    { ar.Serialize(prop.name, *(i64*)ptr); }
    else if (typeName == "u64")    { ar.Serialize(prop.name, *(u64*)ptr); }
    else if (typeName == "f32")    { ar.Serialize(prop.name, *(f32*)ptr); }
    else if (typeName == "f64")    { ar.Serialize(prop.name, *(f64*)ptr); }
    else if (typeName == "String") { ar.Serialize(prop.name, *(String*)ptr); }
    else if (typeName == "float3") { ar.Serialize(prop.name, *(float3*)ptr); }
    else if (typeName == "float4") { ar.Serialize(prop.name, *(float4*)ptr); }
    else if (typeName == "quat")   { ar.Serialize(prop.name, *(quat*)ptr); }
    // else: 跳过未知类型
}

// ============================================================
// Save — 场景保存
// ============================================================
bool SceneSerializer::Save(StringView filePath, World& world, SceneGraph& sg) {
    std::vector<u8> buffer;
    auto w32 = [&](u32 v) { for(int i=0;i<4;++i) buffer.push_back((u8)(v>>(i*8))); };
    auto w64 = [&](u64 v) { for(int i=0;i<8;++i) buffer.push_back((u8)(v>>(i*8))); };

    w32(kMagic); w32(kVersion);

    // --- Entities ---
    u32 ec = 0; world.ForEachEntity([&](Entity){ec++;}); w32(ec);
    world.ForEachEntity([&](Entity e) {
        w64(e.id);
        // 收集组件数据
        struct CD { u64 hash; std::vector<u8> data; };
        std::vector<CD> comps;
        world.ForEachComponent(e, [&](Component* comp) {
            auto* cls = comp->GetClass(); if(!cls) return;
            serialize::BinaryArchive ar(serialize::ArchiveMode::Write);
            ar.BeginObject("comp");
            reflect::ForEachProperty<Component>(comp, [&](const reflect::PropertyInfo& p, void* ptr) {
                if(p.flags & reflect::PF_Serializable) SerializeProperty(ar, p.typeName, p, ptr);
            });
            ar.EndObject();
            comps.push_back({cls->typeHash, ar.GetBuffer()});
        });
        w32((u32)comps.size());
        for(auto& [h,d] : comps) { w64(h); w32((u32)d.size()); buffer.insert(buffer.end(),d.begin(),d.end()); }
    });

    // --- Hierarchy ---
    struct P { u64 c,p; }; std::vector<P> prs;
    world.ForEachEntity([&](Entity e) { Entity p = sg.GetParent(e);
        if(p.IsValid()) prs.push_back({e.id,p.id}); });
    w32((u32)prs.size());
    for(auto& [c,p] : prs) { w64(c); w64(p); }

    // --- 写入文件 ---
    std::ofstream f(String(filePath),std::ios::binary);
    if(!f) { HE_CORE_ERROR("SceneSerializer: write failed: {}", filePath); return false; }
    f.write((const char*)buffer.data(), buffer.size());
    HE_CORE_INFO("Scene saved: {} entities, {} pairs, {} bytes", ec, prs.size(), buffer.size());
    return true;
}

// ============================================================
// Load — 场景加载
// ============================================================
bool SceneSerializer::Load(StringView filePath, World& world, SceneGraph& sg) {
    std::ifstream f(String(filePath),std::ios::binary|std::ios::ate);
    if(!f) { HE_CORE_ERROR("SceneSerializer: open failed: {}", filePath); return false; }
    std::vector<u8> buf((usize)f.tellg()); f.seekg(0); f.read((char*)buf.data(),buf.size());
    usize p=0;
    auto r32=[&](){u32 v=0;for(int i=0;i<4;++i)v|=(u32)buf[p++]<<(i*8);return v;};
    auto r64=[&](){u64 v=0;for(int i=0;i<8;++i)v|=(u64)buf[p++]<<(i*8);return v;};

    if(r32()!=kMagic){ HE_CORE_ERROR("SceneSerializer: bad magic"); return false; }
    if(r32()!=kVersion){ HE_CORE_ERROR("SceneSerializer: bad version"); return false; }

    u32 ec=r32();

    // --- 清除当前世界内容 ---
    std::vector<Entity> toRemove;
    world.ForEachEntity([&](Entity e) { toRemove.push_back(e); });
    for (auto& e : toRemove) world.DestroyEntity(e);

    // --- 重建实体，保留旧 ID 到新 Entity 的映射 ---
    std::unordered_map<u64, Entity> idMap;
    for(u32 ei=0;ei<ec;++ei){
        u64 eid=r64();
        Entity ent=world.CreateEntity("Entity");
        idMap[eid] = ent;

        u32 cc=r32();
        for(u32 ci=0;ci<cc;++ci){
            u64 hash=r64(); u32 ds=r32();

            auto* cls=reflect::TypeRegistry::Instance().FindClassByHash(hash);
            if(!cls){ p+=ds; continue; }

            // 按类型名创建组件（通过 AddComponent）
            StringView cn=cls->name;
            Component* added=nullptr;
            if      (cn=="TransformComponent")  added=world.AddComponent<TransformComponent>(ent);
            else if (cn=="MeshComponent")       added=world.AddComponent<MeshComponent>(ent);
            else if (cn=="CubeComponent")       added=world.AddComponent<CubeComponent>(ent);
            else if (cn=="SphereComponent")     added=world.AddComponent<SphereComponent>(ent);
            else if (cn=="DirectionalLight")    added=world.AddComponent<DirectionalLight>(ent);
            else if (cn=="PointLight")          added=world.AddComponent<PointLight>(ent);
            else if (cn=="SpotLight")           added=world.AddComponent<SpotLight>(ent);
            else if (cn=="LightComponent")      added=world.AddComponent<LightComponent>(ent);
            else { p+=ds; continue; }

            // 反序列化属性到已创建的组件
            serialize::BinaryArchive ar(serialize::ArchiveMode::Read);
            ar.SetBuffer(std::vector<u8>(buf.begin()+p,buf.begin()+p+ds));
            ar.BeginObject("comp");
            reflect::ForEachProperty<Component>(added, [&](const reflect::PropertyInfo& pr, void* ptr) {
                if(pr.flags & reflect::PF_Serializable) SerializeProperty(ar, pr.typeName, pr, ptr);
            });
            ar.EndObject();
            p+=ds;
        }
    }

    // Hierarchy — 通过 idMap 将保存的旧 ID 转换为新创建的 Entity
    u32 pc=r32();
    for(u32 pi=0;pi<pc;++pi){ u64 c=r64(),p=r64(); sg.SetParent(idMap[c], idMap[p]); }

    HE_CORE_INFO("Scene loaded: {} entities", ec);
    return true;
}

} // namespace he::editor
