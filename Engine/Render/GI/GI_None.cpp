#include "GI/GI_None.h"

namespace he::render {

bool GI_None::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    (void)device; (void)width; (void)height;
    return true; // 空实现：无需创建任何资源
}

void GI_None::Shutdown() {
    // 空实现：无资源需要释放
}

void GI_None::Update(const SubsystemContext& ctx) {
    (void)ctx; // 空实现：不收集任何场景数据
}

void GI_None::Render(rhi::IRHICommandList* cmdList) {
    (void)cmdList; // 空实现：不录制任何 GPU 命令
}

void GI_None::Bind(rhi::IRHICommandList* cmdList) const {
    (void)cmdList; // 空实现：不绑定任何描述符集
}

void GI_None::OnResize(u32 width, u32 height) {
    (void)width; (void)height; // 空实现：无分辨率相关资源
}

} // namespace he::render
