import { UIAbility } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

// ============================================================
// EntryAbility —— 鸿蒙应用入口（Stage 模型）
//
// 只负责页面装载：真正的渲染在 native（libentry.so）里，
// 由 pages/Index.ets 的 XComponent 在 surface 就绪后调用 native.init。
// ============================================================

const TAG = 'HugEngine';

export default class EntryAbility extends UIAbility {
  onCreate(): void {
    hilog.info(0x0000, TAG, 'EntryAbility onCreate');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, TAG, 'loadContent 失败: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(0x0000, TAG, 'Index 页面已加载');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(0x0000, TAG, 'onWindowStageDestroy');
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, 'onDestroy');
  }
}
