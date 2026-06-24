# Reflect (L1)

宏驱动反射系统 — HE_CLASS / HE_REGISTER_PROPERTY / HE_ATTR_* 宏注册方案。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Meta | `^T` 反射操作符封装、`meta::info` 类型擦除、`[:...:]` Splicer | P1 |
| Attribute | `[[engine::category]]`, `[[engine::range]]`, `[[engine::tooltip]]` 等 13 种标准属性 | P1 |
| Serialize | 编译期生成 Binary + JSON 序列化，`meta::members_of` 自动展开 | P1 |

**依赖**: Core
**关键接口**: `TypeRegistry`, `PropertyDescriptor`, `for_each_property<T>()`
