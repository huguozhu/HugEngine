#pragma once

#include <memory>

namespace he::rhi {

// ============================================================
// IRHIQueryPool — GPU 查询池（Timestamp / PipelineStatistics）
// ============================================================
class IRHIQueryPool {
public:
    virtual ~IRHIQueryPool() = default;
    virtual u32 GetQueryCount() const = 0;
    virtual QueryType GetQueryType() const { return QueryType::Timestamp; }
};

} // namespace he::rhi
