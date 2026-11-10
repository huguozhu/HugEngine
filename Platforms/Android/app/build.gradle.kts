// ============================================================
// Platforms/Android/app/build.gradle.kts
//
// 关键点：
//   · externalNativeBuild 指向**仓库根**的 CMakeLists（引擎/样例/入口同一套构建）
//   · -DHUGENGINE_BUILD_ANDROID_ENTRY=ON 让根 CMake 只编 Android 入口，不编桌面样例
//   · abiFilters 只留 arm64-v8a（与鸿蒙保持一致；x86_64 留给模拟器时再加）
// ============================================================
plugins {
    id("com.android.application")
}

android {
    namespace = "com.hugengine.sample"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.hugengine.sample"
        // 【必须 ≥33】VMA 的静态函数表会引用 Vulkan 1.3 的 vkGetDevice{Buffer,Image}MemoryRequirements，
        // NDK 的 libvulkan.so 只有 API 33+ 才导出它们（1.2 的 vkGetBufferDeviceAddress 需要 31+）。
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

        ndk {
            // arm64 真机 + x86_64 模拟器（本机模拟器镜像是 x86_64）
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // 与顶层 CMake 的平台开关一一对应；移动端不建桌面样例/D3D/物理。
                // 【必须有 EDITOR=ON】样例会 include Editor/ImGuiIntegration.h，编辑器的
                // CMake 在 OFF 时整个子目录都不加入 ⇒ HugEngineEditor 目标不存在（鸿蒙同理）。
                arguments += listOf(
                    "-DHUGENGINE_BUILD_ANDROID_ENTRY=ON",
                    "-DHUGENGINE_BUILD_SAMPLES=OFF",
                    "-DHUGENGINE_BUILD_EDITOR=ON",
                    "-DHUGENGINE_BUILD_TESTS=OFF",
                    "-DHUGENGINE_RHI_D3D12=OFF",
                    "-DHUGENGINE_RHI_D3D11=OFF",
                    "-DANDROID_STL=c++_shared"
                )
                // 引擎用 C++20（与桌面/鸿蒙一致）
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            // 注意：从 app/ 到仓库根是三级（app → Android → Platforms → 根）
            path = file("../../../CMakeLists.txt")
            version = "3.31.5"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
        }
    }

    // 大资产（Sponza 等）不随包；01/02 只需要天空盒贴图，由 sync-assets.ps1 同步
    androidResources {
        noCompress += listOf("png", "glb", "gltf", "hdr", "ttf", "otf")
    }

    packaging {
        jniLibs {
            // libc++_shared.so 由 NDK 提供，必须随包分发
            useLegacyPackaging = true
        }
    }
}

dependencies {
    // 首版不引 androidx：SurfaceView + 系统 API 已够用，减少依赖下载与版本约束
}
