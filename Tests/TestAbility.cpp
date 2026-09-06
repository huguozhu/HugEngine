// ============================================================
// Tests/TestAbility.cpp — Phase B4 AbilityComponent 单元测试
//
// 覆盖：技能注册/查找、施放（消耗/冷却/回调）、冷却计时、
//       资源不足拒绝、Agent 动作 CastAbility（编译/执行/撤销）。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Scene/SceneGraph.h"
#include "Scene/AbilityComponent.h"
#include "Scene/AbilitySystem.h"
#include "AI/WorldModel/Action.h"
#include "Editor/Command.h"

using namespace he;
using namespace he::ai;

TEST_CASE("AbilityComponent 技能注册与查找") {
    World world;
    Entity e = world.CreateEntity("Mage");
    world.AddComponent<TransformComponent>(e);
    auto* ability = world.AddComponent<AbilityComponent>(e);

    int fire = ability->AddSkill("Fireball", 3.0f, 10.0f);
    int heal = ability->AddSkill("Heal", 5.0f, 20.0f);
    CHECK(fire == 0);
    CHECK(heal == 1);
    CHECK(ability->skills.size() == 2);
    CHECK(ability->cooldownRemaining.size() == 2);   // 冷却槽与技能对齐

    // 幂等注册：同名返回已有下标，不新增
    CHECK(ability->AddSkill("Fireball", 99.0f, 99.0f) == fire);
    CHECK(ability->skills.size() == 2);
    CHECK(ability->skills[fire].cooldown == doctest::Approx(3.0f));   // 原定义不被覆盖

    CHECK(ability->FindSkill("Heal") == heal);
    CHECK(ability->FindSkill("Blink") == -1);
}

TEST_CASE("AbilityComponent 施放：消耗/冷却/回调") {
    World world;
    Entity e = world.CreateEntity("Mage");
    world.AddComponent<TransformComponent>(e);
    auto* ability = world.AddComponent<AbilityComponent>(e);
    int fire = ability->AddSkill("Fireball", 3.0f, 10.0f);

    Entity castCaster = Entity{kInvalidEntity};
    String castName;
    float3 castTarget(0.0f);
    ability->onCast = [&](Entity caster, const SkillDef& s, const float3& target) {
        castCaster = caster;
        castName   = s.name;
        castTarget = target;
    };

    // 施放成功：扣资源 100→90，进冷却 3 秒，回调收到 caster/技能/目标
    CHECK(ability->Cast(fire, float3(1, 2, 3)) == true);
    CHECK(ability->resource == doctest::Approx(90.0f));
    CHECK(ability->cooldownRemaining[fire] == doctest::Approx(3.0f));
    CHECK(castCaster == e);
    CHECK(castName.find("Fireball") != String::npos);
    CHECK(castTarget.x == doctest::Approx(1.0f));

    // 冷却中不可施放：无副作用
    CHECK(ability->CanCast(fire) == false);
    CHECK(ability->Cast(fire, float3(0)) == false);
    CHECK(ability->resource == doctest::Approx(90.0f));   // 未再扣
}

TEST_CASE("AbilitySystem 冷却计时递减到可施放") {
    World world;
    Entity e = world.CreateEntity("Mage");
    world.AddComponent<TransformComponent>(e);
    auto* ability = world.AddComponent<AbilityComponent>(e);
    int fire = ability->AddSkill("Fireball", 2.0f, 10.0f);

    ability->Cast(fire, float3(0));
    CHECK(ability->CanCast(fire) == false);

    // 1.5 秒后仍冷却；2.5 秒后冷却结束（钳制到 0）
    AbilitySystem::Update(world, 1.5f);
    CHECK(ability->CanCast(fire) == false);
    CHECK(ability->cooldownRemaining[fire] == doctest::Approx(0.5f));
    AbilitySystem::Update(world, 2.5f);
    CHECK(ability->cooldownRemaining[fire] == doctest::Approx(0.0f));   // 钳制
    CHECK(ability->CanCast(fire) == true);
}

TEST_CASE("AbilityComponent 资源不足拒绝施放") {
    World world;
    Entity e = world.CreateEntity("Mage");
    world.AddComponent<TransformComponent>(e);
    auto* ability = world.AddComponent<AbilityComponent>(e);
    int big = ability->AddSkill("Meteor", 1.0f, 500.0f);

    CHECK(ability->CanCast(big) == false);
    CHECK(ability->Cast(big, float3(0)) == false);
    CHECK(ability->resource == doctest::Approx(100.0f));   // 未扣资源
    CHECK(ability->cooldownRemaining[big] == doctest::Approx(0.0f));   // 未进冷却

    // 越界下标安全
    CHECK(ability->CanCast(99) == false);
    CHECK(ability->Cast(-1, float3(0)) == false);
}

TEST_CASE("Agent 动作 CastAbility 编译/执行/撤销") {
    World world;
    SceneGraph sg(world);

    Entity mage = world.CreateEntity("Mage");
    world.AddComponent<TransformComponent>(mage);
    auto* ability = world.AddComponent<AbilityComponent>(mage);
    ability->AddSkill("Fireball", 3.0f, 10.0f);

    int castCount = 0;
    ability->onCast = [&](Entity, const SkillDef&, const float3&) { ++castCount; };

    // 合法动作 → 编译成功 → 执行 → 资源扣除 + 回调触发
    Action a;
    a.op = "CastAbility";
    a.targetEntity = mage.id;
    a.argsJson = R"({"skill":"Fireball","target":[1,0,0]})";
    auto cmd = CompileAction(world, sg, a);
    REQUIRE(cmd != nullptr);
    cmd->Execute();
    CHECK(castCount == 1);
    CHECK(ability->resource == doctest::Approx(90.0f));
    CHECK(ability->CanCast(0) == false);

    // 撤销 → 资源与冷却恢复
    cmd->Undo();
    CHECK(ability->resource == doctest::Approx(100.0f));
    CHECK(ability->CanCast(0) == true);

    // 冷却中（重新施放后）→ 编译失败返回 nullptr
    cmd->Execute();
    auto cmd2 = CompileAction(world, sg, a);
    CHECK(cmd2 == nullptr);

    // 技能不存在 / 无组件实体 → nullptr
    Action bad;
    bad.op = "CastAbility";
    bad.targetEntity = mage.id;
    bad.argsJson = R"({"skill":"Blink"})";
    CHECK(CompileAction(world, sg, bad).get() == nullptr);   // .get()：doctest 无法按值存 unique_ptr
    Entity noAbility = world.CreateEntity("Cube");
    world.AddComponent<TransformComponent>(noAbility);
    bad.targetEntity = noAbility.id;
    bad.argsJson = R"({"skill":"Fireball"})";
    CHECK(CompileAction(world, sg, bad).get() == nullptr);
}
