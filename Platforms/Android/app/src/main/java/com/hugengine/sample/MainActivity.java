package com.hugengine.sample;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;

/**
 * HugEngine Android 入口（对应鸿蒙的 Index.ets 页面）。
 *
 * 职责：
 *   1) 用 SurfaceView 提供绘制表面（surfaceCreated/Changed/Destroyed → JNI）；
 *   2) 把触摸事件归一化成 [0,1] 后转给 native（契约见 he::PushTouchPoint）；
 *   3) 顶部按钮切换样例（1 = 01.Triangle，2 = 02.Cube）。
 *
 * 渲染循环与样例分发在 native 侧（Samples/Common/App/MobileHost），与鸿蒙共用。
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback, View.OnTouchListener {

    private static final int SAMPLE_TRIANGLE = 1;
    private static final int SAMPLE_CUBE     = 2;

    private SurfaceView mSurfaceView;
    private View        mButtons;

    // ---- JNI：实现见 Platforms/Android/app/src/main/cpp/NativeEntry.cpp ----
    // 【必须显式加载】libhgengine.so 依赖 libc++_shared.so，先加载 C++ 运行库再加载引擎库，
    // 否则会在 surfaceCreated 里报 UnsatisfiedLinkError（首次真机实测就是漏了这一步）。
    static {
        System.loadLibrary("c++_shared");
        System.loadLibrary("hgengine");
    }

    private static native boolean nativeInit(Surface surface, int width, int height,
                                             String filesDir, Object assetManager, float density);
    private static native void nativeStart(int mode);
    private static native void nativeResize(int width, int height);
    private static native void nativeTouch(int id, int phase, float xNorm, float yNorm);
    private static native boolean nativeScreenshot(String path);
    private static native void nativeShutdown();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // 简易布局：顶部一行样例按钮 + 占据剩余空间的 SurfaceView（无需 XML）
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(12, 12, 12, 8);
        mButtons = bar;

        bar.addView(makeButton("01 三角形", () -> switchTo(SAMPLE_TRIANGLE)));
        bar.addView(makeButton("02 立方体", () -> switchTo(SAMPLE_CUBE)));

        mSurfaceView = new SurfaceView(this);
        mSurfaceView.getHolder().addCallback(this);
        mSurfaceView.setOnTouchListener(this);

        root.addView(bar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(mSurfaceView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        setContentView(root);
    }

    /** 生成一个样例按钮（点击后由 native 重建整个 App） */
    private Button makeButton(String text, Runnable onClick) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        button.setOnClickListener(v -> onClick.run());
        return button;
    }

    private void switchTo(int mode) {
        nativeStart(mode);
    }

    // ---------------- SurfaceHolder.Callback ----------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        final Surface surface = holder.getSurface();
        final android.graphics.Rect frame = holder.getSurfaceFrame();
        final float density = getResources().getDisplayMetrics().density;
        final boolean ok = nativeInit(surface, frame.width(), frame.height(),
                getFilesDir().getAbsolutePath(), getAssets(), density);
        if (!ok) {
            throw new RuntimeException("HugEngine: nativeInit 失败");
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeResize(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeShutdown();
    }

    // ---------------- 触摸 ----------------

    @Override
    public boolean onTouch(View view, MotionEvent event) {
        final float w = Math.max(1.0f, view.getWidth());
        final float h = Math.max(1.0f, view.getHeight());
        // 【坐标契约】native 侧只接受归一化坐标（鸿蒙曾因多除一次导致触摸恒为 0）
        final float nx = event.getX() / w;
        final float ny = event.getY() / h;

        final int phase;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:   phase = 0; break;   // Began
            case MotionEvent.ACTION_MOVE:   phase = 1; break;   // Moved
            case MotionEvent.ACTION_UP:     phase = 2; break;   // Ended
            case MotionEvent.ACTION_CANCEL: phase = 3; break;   // Cancelled
            default: return false;
        }
        // 只跟踪第一根手指：引擎当前的单指相机方案（多指手势后续再加）
        nativeTouch(0, phase, nx, ny);
        return true;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        nativeShutdown();
    }
}
