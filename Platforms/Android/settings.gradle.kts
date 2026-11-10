// ============================================================
// Platforms/Android/settings.gradle.kts
//
// HugEngine Android 工程（AGP + CMake externalNativeBuild）。
// 仓库根 CMakeLists 同时被 Windows / 鸿蒙 / Android 复用：
//   · Android 通过 app/build.gradle.kts 传 -DHUGENGINE_BUILD_ANDROID_ENTRY=ON
//   · 引擎与样例源码一行不改，只有入口（Platforms/Android）是新增的
// ============================================================
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "HugEngine"
include(":app")
