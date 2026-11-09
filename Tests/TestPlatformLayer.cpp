// ============================================================
// TestPlatformLayer.cpp —— 平台层（P0）契约测试
//
// 覆盖与平台无关、但很容易被改坏的几条约定：
//   1. vfs::Resolve / ResolveWritable：相对路径挂根、绝对路径直通、可写目录自动创建
//   2. Time::Now：单调不减
//   3. InputState：按下/刚按下语义（CameraInput 与 ImGui 后端都依赖它）
//
// 不创建真实窗口（CI 无显示），窗口行为由 01/02 的实跑回归覆盖。
// ============================================================

#include "doctest.h"

#include "Platform/IInput.h"
#include "Platform/IVfs.h"
#include "Platform/Time.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

using namespace he;

TEST_CASE("Platform/Vfs: 相对路径挂在资产根下") {
    vfs::ResetRoots();
    vfs::SetRoots("D:/tmp/hugengine_assets", "D:/tmp/hugengine_write");

    const String resolved = vfs::Resolve("gltf/Sponza/glTF/Sponza.gltf");
    CHECK(resolved.find("hugengine_assets") != String::npos);
    CHECK(resolved.find("gltf") != String::npos);
    CHECK(resolved.find("Sponza.gltf") != String::npos);

    // 绝对路径原样返回（不受根目录影响）
    const String abs = "D:/somewhere/else/file.png";
    CHECK(vfs::Resolve(abs) == abs);
}

TEST_CASE("Platform/Vfs: 可写路径挂在可写根下并自动建目录") {
    const fs::path root = fs::current_path() / "test_tmp" / "vfs_writable";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    vfs::ResetRoots();
    vfs::SetRoots((root / "assets").string(), (root / "write").string());

    const String out = vfs::ResolveWritable("Config/deep/test.cfg");
    CHECK(out.find("vfs_writable") != String::npos);   // 根目录名（测试临时目录已从 %TEMP% 改到工作区）
    CHECK(out.find("Config") != String::npos);

    // 父目录应当已被创建：可以直接写文件
    {
        INFO("resolved=" << out);
        INFO("parent exists=" << fs::exists(fs::path(out).parent_path()));
        std::ofstream f(out);
        REQUIRE(f.is_open());
        f << "k=v\n";
    }
    CHECK(fs::exists(fs::path(out)));

    vfs::ResetRoots();
    fs::remove_all(root, ec);
}

TEST_CASE("Platform/Vfs: Exists 区分存在与不存在") {
    const fs::path root = fs::current_path() / "test_tmp" / "vfs_exists";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "content", ec);

    vfs::ResetRoots();
    vfs::SetRoots((root / "content").string(), root.string());

    CHECK_FALSE(vfs::Exists("missing.txt"));

    {
        std::ofstream f((root / "content" / "present.txt"));
        f << "x";
    }
    CHECK(vfs::Exists("present.txt"));

    vfs::ResetRoots();
    fs::remove_all(root, ec);
}

TEST_CASE("Platform/Time: Now 单调不减且能推进") {
    const f64 t0 = Time::Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const f64 t1 = Time::Now();

    CHECK(t1 >= t0);
    CHECK(t1 > t0);   // 5ms 的睡眠必须体现在单调时钟上
}

TEST_CASE("Platform/Input: InputState 的按下/刚按下语义") {
    InputState in;
    for (u32 i = 0; i < kKeyCount; ++i) {
        CHECK_FALSE(in.keys[i]);
        CHECK_FALSE(in.keysPressed[i]);
    }
    CHECK_FALSE(in.IsDown(Key::W));
    CHECK_FALSE(in.IsPressed(Key::W));

    in.keys[static_cast<u32>(Key::W)] = true;
    in.keysPressed[static_cast<u32>(Key::W)] = true;
    CHECK(in.IsDown(Key::W));
    CHECK(in.IsPressed(Key::W));
    CHECK_FALSE(in.IsDown(Key::A));

    // 鼠标三键与触摸点的默认状态
    CHECK_FALSE(in.mouseDown[0]);
    CHECK_FALSE(in.mouseDown[1]);
    CHECK_FALSE(in.mouseDown[2]);
    CHECK(in.touchCount == 0u);
}
