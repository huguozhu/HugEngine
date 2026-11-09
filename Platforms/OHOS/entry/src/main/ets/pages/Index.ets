import native from 'libentry.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { display } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { systemShare } from '@kit.ShareKit';
import { fileUri } from '@kit.CoreFileKit';

// ============================================================
// Index —— 样例切换（01.Triangle / 02.Cube）+ XComponent 渲染 + 触摸转发
//
// 时序：
//   1. XComponent 的 surface 就绪 → onLoad：取 surfaceId 交给 native.init（默认启动 01）
//   2. 顶部两个按钮 → native.start(1|2)：整份重建渲染实例，XComponent 窗口保持不变
//   3. 触摸事件 → native.touch（**归一化坐标**，与 native 侧 TouchPhase 对齐）
//   4. surface 销毁 → native.shutdown；尺寸变化 → native.resize
//
// 【触摸为什么挂在渲染区域的 Column 上】XComponent(SURFACE) 会自己消费触摸事件
// （实测其 onTouch 从未触发），因此给它设 hitTestBehavior(HitTestMode.None)，
// 由外层容器统一接收再转给 native。
// ============================================================

const TAG = 'HugEngine';

@Entry
@Component
struct Index {
  private controller: XComponentController = new XComponentController();
  private ready: boolean = false;
  // 最近一次渲染区域尺寸（vp），用于把触摸点归一化
  private viewW: number = 1;
  private viewH: number = 1;
  // 诊断：只打第一条触摸日志，避免刷屏
  private touchLogged: boolean = false;
  // 当前样例：1 = 01.Triangle，2 = 02.Cube，3 = 03.Sponza，4 = 04.Deferred
  @State private mode: number = 1;

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

  /// 切换样例：native 侧停旧实例、起新实例（surface 不变）
  private switchTo(mode: number): void {
    if (!this.ready || this.mode === mode) return;
    hilog.info(0x0000, TAG, '切换样例: %{public}d', mode);
    native.start(mode);
    this.mode = mode;
  }

  /// 分享当前画面：native 截下一帧 → 等落盘 → 调系统分享面板
  private async shareScreenshot(): Promise<void> {
    if (!this.ready) return;
    const ctx = getContext(this) as common.UIAbilityContext;
    const path = ctx.filesDir + '/share_shot.png';
    if (!native.screenshot(path)) {
      hilog.error(0x0000, TAG, '截图请求失败');
      return;
    }
    // 截帧发生在下一帧 Present，等它落盘（60FPS 下 400ms 足够）
    await new Promise<void>((resolve) => setTimeout(() => resolve(), 400));
    try {
      // 【必须用文档 URI】直接把 file:///data/... 交给分享面板，别的应用没有访问权，
      // 分享会失败（实测错误码 1003702002）。fileUri.getUriFromPath 会生成可授权的 URI。
      const uri = fileUri.getUriFromPath(path);
      const data = new systemShare.SharedData({ utd: 'general.png', uri });
      const controller = new systemShare.ShareController(data);
      await controller.show(ctx, {
        selectionMode: systemShare.SelectionMode.SINGLE,
        previewMode: systemShare.SharePreviewMode.DETAIL,
      });
      hilog.info(0x0000, TAG, '分享面板已展示');
    } catch (err) {
      hilog.error(0x0000, TAG, '分享失败: %{public}s', JSON.stringify(err));
    }
  }

  build() {
    Column() {
      // ---- 顶部样例切换（不参与渲染区域的触摸转发）----
      // 4 个样例按钮：屏幕宽度不够时自动换行（Flex wrap），避免被挤出屏幕
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
        Button('01 三角形')
          .type(ButtonType.Capsule)
          .height(36)
          .fontSize(14)
          .margin({ right: 6, bottom: 6 })
          .backgroundColor(this.mode === 1 ? '#3A6FD8' : '#444444')
          .onClick(() => this.switchTo(1))
        Button('02 立方体')
          .type(ButtonType.Capsule)
          .height(36)
          .fontSize(14)
          .margin({ right: 6, bottom: 6 })
          .backgroundColor(this.mode === 2 ? '#3A6FD8' : '#444444')
          .onClick(() => this.switchTo(2))
        Button('03 Sponza')
          .type(ButtonType.Capsule)
          .height(36)
          .fontSize(14)
          .margin({ right: 6, bottom: 6 })
          .backgroundColor(this.mode === 3 ? '#3A6FD8' : '#444444')
          .onClick(() => this.switchTo(3))
        Button('04 延迟')
          .type(ButtonType.Capsule)
          .height(36)
          .fontSize(14)
          .margin({ right: 6, bottom: 6 })
          .backgroundColor(this.mode === 4 ? '#3A6FD8' : '#444444')
          .onClick(() => this.switchTo(4))
        Button('分享截图')
          .type(ButtonType.Capsule)
          .height(36)
          .fontSize(14)
          .margin({ right: 6, bottom: 6 })
          .backgroundColor('#2E7D32')
          .onClick(() => { this.shareScreenshot(); })
      }
      .width('100%')
      .padding({ left: 12, right: 12, top: 8, bottom: 8 })
      .backgroundColor('#1A1A1A')

      // ---- 渲染区域（触摸在此转发给 native）----
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
      .layoutWeight(1)
      .backgroundColor(Color.Black)
      .onTouch((event: TouchEvent) => {
        this.handleTouch(event);
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
  }
}
