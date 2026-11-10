// ============================================================
// Platforms/Android/build.gradle.kts（根工程）
//
// 只负责声明插件版本；具体配置在 app/build.gradle.kts。
// AGP 8.13 需要 Gradle 8.13+（本仓库用 8.14.x）。
// ============================================================
plugins {
    id("com.android.application") version "8.13.0" apply false
}
