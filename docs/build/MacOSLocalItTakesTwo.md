# Apple Silicon 本地构建与《双人成行》验证记录

这份说明对应 `fix/macos-it-takes-two` 工作分支，基于 Eden `v0.2.1`
（`58c1e20ee58efa3900ba616207d460886214480b`）。这是本地开发与验证记录，
实际双人关卡仍需继续验证。

## 修改后重新构建

先按照 [Deps.md](../Deps.md) 准备构建依赖，再在仓库根目录执行：

```sh
bash tools/build-macos-local.sh
```

脚本会构建 Qt 应用，并运行 ARM64 Dynarmic 常规测试。应用位于：

```text
build/macos-core/bin/eden.app
```

这是供本机开发使用的构建，仍依赖 Homebrew 动态库和已安装 Eden 的 MoltenVK。
直接复制应用到另一台电脑还需要另外处理依赖打包。

验证环境：M5 Max、128 GB 内存、macOS 26.6.2、AppleClang 21、Qt 6.11.2。
构建脚本使用 `.cache/build-tools` 中的 CMake/Ninja、
`.cache/system-deps/boost/1.92.0` 中的静态 Boost，以及
`/Applications/eden.app/Contents/Frameworks/libMoltenVK.dylib`。
可通过 `EDEN_CMAKE`、`EDEN_NINJA`、`EDEN_BOOST_PREFIX`、
`EDEN_MOLTENVK_LIBRARY`、`EDEN_BUILD_DIR` 和 `EDEN_JOBS` 覆盖这些选择。
完整依赖说明见 [Deps.md](../Deps.md)。

## 使用独立配置运行

```sh
bash tools/run-macos-local.sh "/绝对路径/游戏.xci"
```

不传游戏路径时打开本地构建的游戏列表：

```sh
bash tools/run-macos-local.sh
```

脚本使用 `.cache/game-test/config` 作为 `XDG_CONFIG_HOME`。
首次运行时只复制原有主配置，后续修改写入这份独立配置。
原有 `/Applications/eden.app` 和 `~/.config/eden` 配置不会被替换。
游戏数据、存档、固件、密钥和着色器缓存仍使用既有数据目录，存档没有隔离。
可用 `EDEN_TEST_CONFIG_DIR` 指定另一份配置目录。
通过 Finder 直接打开应用不会自动使用这份独立配置，因此测试请使用脚本。

测试设置为 Vulkan / MoltenVK、1 倍分辨率、多核、CPU Accurate、软件 NVDEC。
程序导出和 GDB 调试已关闭。请使用英文输入法操作键盘映射，或在
Preferences → Controls 中为玩家一、玩家二分别选择控制器，并启用两个连接。

本机验证用的独立配置启用了两个虚拟 Pro Controller，使用以下键盘映射。
这份配置不随源码提交；其他机器需自行设置：

| 操作 | 玩家一 | 玩家二 |
| --- | --- | --- |
| A / 确认 | Return | Space |
| B / 返回 | Backspace | O |
| 左摇杆 | WASD | 方向键 |
| 右摇杆 | IJKL | 8、5、4、6 |
| + / 开始 | M | 0 |

普通按键需实际按住一小段时间，自动化的瞬时按键可能赶不上游戏的轮询。
验证过程中临时使用过切换式确认键，现已恢复为按下/松开方式。
正式双人游玩建议分别映射两个实体手柄。

## 源码修改及原因

- `src/yuzu/main_window.cpp` 和 `src/yuzu/game/game_list*`：
  游戏启动前等待游戏列表扫描完成。扫描会清空、重建共享内容索引；
  之前命令行直接启动可能选中 1.0.0 主程序，随后又加载 1.0.2 资源。
  完成状态可重复等待，通知期间由互斥锁保护，保证启动和析构都能等待。
  游戏库很大时，启动会等待扫描，因此可能出现额外延迟。
- `src/yuzu/main.cpp`：macOS 默认启用 `QT_MTL_NO_TRANSACTION=1`，
  避免 Qt 6.11 的 Metal layer 事务管理与 MoltenVK 自行呈现冲突。
  已设置的环境变量值优先。
- `src/dynarmic/src/dynarmic/backend/arm64/{address_space,emit_arm64}.{h,cpp}`：
  ARM64 发射过程引用原有 IR Block，避免移动其内联指令存储后留下失效指针。
  本地未修改的 CPU 源码也避开了官方应用的早期崩溃，因此不能将这个改动
  单独认定为官方崩溃的全部原因。
- `src/common/settings_setting.h`：包含 `fmt/format.h`，兼容本机 fmt 12。

## 已验证范围（2026-10-06）

- Qt 应用构建成功；ARM64 常规测试通过：87 个测试、1681 条断言。
- `git diff --check` 和两个脚本的 `bash -n` 检查通过。
- 《双人成行》1.0.2 的 ExeFS 和 RomFS 更新均正确应用。
  更新后的 main Build ID 为 `C4067E8CB3258656D8E9B448EE31AA38`。
- 游戏通过开场、加载、标题画面，进入首次语言设置和 EA 用户协议页面。
  标题和这些设置页约 30 FPS；最终构建已再次启动到标题画面。
  窗口可以关闭并重新启动；一次关闭窗口后进程仍停留在主事件循环，
  已结束该测试进程，正常退出仍需进一步回归。
- EA 用户协议的接受需要用户决定；目前没有验证本地双人关卡、
  3D 场景的画面正确性、实际游戏帧率或长期稳定性。

日志仍出现 `Geometry streams is not implemented`。
MoltenVK 的几何着色器能力有限，是否影响具体场景必须继续实际验证。
标题画面的 30 FPS 不代表完整游戏已经可玩。

构建和诊断日志保存在 `.cache/diagnostics`；不应把游戏内容、密钥、
存档或程序导出加入源码提交。临时调试器、游戏资源补丁和 Unicorn 测试修改
均已撤回。
