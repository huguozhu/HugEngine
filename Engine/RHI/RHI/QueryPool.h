#pragma once

#include <memory>

namespace he::rhi {

// ============================================================
// IRHIQueryPool — GPU 时间戳查询池
// ============================================================
class IRHIQueryPool {
public:
    virtual ~IRHIQueryPool() = default;
    virtual u32 GetQueryCount() const = 0;
};

} // namespace he::rhi
