# Reflect (L1)

C++26 静态反射系统 — 零宏、零外部代码生成器的反射方案。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Meta | `^T` 反射操作符封装、`meta::info` 类型擦除、`[:...:]` Splicer | P1 |
| Attribute | `[[engine::category]]`, `[[engine::range]]`, `[[engine::tooltip]]` 等 13 种标准属性 | P1 |
| Serialize | 编译期生成 Binary + JSON 序列化，`meta::members_of` 自动展开 | P1 |

**依赖**: Core
**关键接口**: `TypeRegistry`, `PropertyDescriptor`, `for_each_property<T>()`
