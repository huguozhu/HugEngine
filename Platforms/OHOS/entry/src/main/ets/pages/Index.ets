import native from 'libentry.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { display } from '@kit.ArkUI';

// ============================================================
// Index —— XComponent(surface) + 触摸转发
//
// 时序：
//   1. XComponent 的 surface 就绪 → onLoad：取 surfaceId 交给 native.init
//   2. 触摸事件 → native.touch（**归一化坐标**，与 native 侧 TouchPhase 对齐）
//   3. surface 销毁 → native.shutdown（停渲染线程、断开窗口）
//   4. 尺寸变化（旋转/分屏）→ native.resize（native 重建交换链 + 重设窗口缓冲几何）
//
// 【触摸为什么挂在 Column 上】XComponent(SURFACE) 会自己消费触摸事件（实测其 onTouch
// 从未触发），因此给它设 hitTestBehavior(HitTestMode.None)，由外层 Column 统一接收，
// 再转成归一化坐标交给 native。
// ============================================================

const TAG = 'HugEngine';

@Entry
@Component
struct Index {
  private controller: XComponentController = new XComponentController();
  private ready: boolean = false;
  // 最近一次组件尺寸（vp），用于把触摸点归一化
  private viewW: number = 1;
  private viewH: number = 1;
  // 诊断：只打第一条触摸日志，避免刷屏
  private touchLogged: boolean = false;

  /// 把 ArkUI 的触摸事件转成 native 的 (phase, 归一化坐标)
  private handleTouch(event: TouchEvent): void {
    if (!this.touchLogged) {
      this.touchLogged = true;
      hilog.info(0x0000, TAG, 'ArkTS onTouch 首次: type=%{public}d fingers=%{public}d',
                 event.type, event.touches.length);
    }
    if (!this.ready) return;

    const t = event.touches[0];
    if (!t) return;

    // phase 与 native 的 he::ohos::TouchPhase 对齐：0=Began 1=Moved 2=Ended 3=Cancelled
    let phase = 1;
    if (event.type === TouchType.Down) phase = 0;
    else if (event.type === TouchType.Up) phase = 2;
    else if (event.type === TouchType.Cancel) phase = 3;

    const nx = this.viewW > 0 ? t.x / this.viewW : 0;
    const ny = this.viewH > 0 ? t.y / this.viewH : 0;
    // 【必须传 0】native 只跟踪第一个手指（id==0）；ArkTS 的 TouchObject.id 不保证是 0
    native.touch(0, phase, nx, ny);
  }

  build() {
    Column() {
      XComponent({
        id: 'hugengine',
        type: XComponentType.SURFACE,
        libraryname: 'entry',
        controller: this.controller
      })
        .width('100%')
        .height('100%')
        // 不参与命中测试：触摸交给外层 Column（见文件头说明）
        .hitTestBehavior(HitTestMode.None)
        .onLoad(() => {
          const surfaceId: string = this.controller.getXComponentSurfaceId();
          const ctx = getContext(this);
          const filesDir: string = ctx.filesDir;
          // 高 DPI：1260px 宽的屏上 16px 字太小，把屏幕密度交给 native 放大 ImGui
          const d = display.getDefaultDisplaySync();
          native.setUiScale(d.densityPixels);
          // 首帧用 960x540 起步；onAreaChange 会按真实尺寸调 native.resize 重建交换链。
          // resourceManager 交给 native，把 rawfile 里的 content/ 解压到沙箱（只解一次）
          native.init(surfaceId, 960, 540, filesDir, ctx.resourceManager);
          this.ready = true;
          hilog.info(0x0000, TAG, 'native.init 完成: surfaceId=%{public}s', surfaceId);
        })
        .onDestroy(() => {
          if (this.ready) {
            native.shutdown();
            this.ready = false;
          }
        })
        .onAreaChange((_old: Area, now: Area) => {
          const w = Number(now.width);
          const h = Number(now.height);
          if (w > 0 && h > 0) {
            this.viewW = w;
            this.viewH = h;
            // 旋转/分屏：native 需要**像素**尺寸，这里把 vp 换算成 px
            if (this.ready) {
              native.resize(Math.round(vp2px(w)), Math.round(vp2px(h)));
            }
          }
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
    .onTouch((event: TouchEvent) => {
      this.handleTouch(event);
    })
  }
}
