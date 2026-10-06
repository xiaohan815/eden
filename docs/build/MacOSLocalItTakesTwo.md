# Apple Silicon 本地构建与《双人成行》验证记录

这份说明对应 `fix/macos-it-takes-two` 工作分支，基于 Eden `v0.2.1`
（`58c1e20ee58efa3900ba616207d460886214480b`）。这是本地开发与验证记录，
已进入本地双人新游戏和开场 3D 场景；目前仍有严重画面异常，
实际双人关卡和长期稳定性仍需继续验证。

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

初次基准设置为 Vulkan / MoltenVK、1 倍分辨率、多核、CPU Accurate、软件 NVDEC。
M5 Max 调整方案见下文；原有主配置不会随本地验证配置改变。
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
- `src/video_core/vulkan_common/{vulkan_device.cpp,memory_budget.h}`：
  MoltenVK 的集成 GPU 在 Aggressive 模式下允许最多 16 GiB 缓存预算。
  先扣除驱动报告的当前用量，再留出 8 GiB 余量；预算不足时自动缩小。
  这是缓存回收阈值，不会预先申请 16 GiB，也不是系统显存硬限制。

## 已验证范围（2026-10-06）

- Qt 应用构建成功；ARM64 常规测试通过：87 个测试、1681 条断言。
- `git diff --check` 和两个脚本的 `bash -n` 检查通过。
- 《双人成行》1.0.2 的 ExeFS 和 RomFS 更新均正确应用。
  更新后的 main Build ID 为 `C4067E8CB3258656D8E9B448EE31AA38`。
- 游戏通过加载、标题画面和主菜单，进入本地新游戏页面，
  两名虚拟控制器均已加入，并进入新游戏的开场 3D 场景和角色选择页面。
  本次启动没有再次出现 EA 用户协议页面。
  2×分辨率、16 GiB 预算下，主菜单和已观察的开场镜头约 30 FPS；
  首次加载新场景时有着色器编译停顿。
  窗口可以关闭并重新启动；一次关闭窗口后进程仍停留在主事件循环，
  已结束该测试进程，正常退出仍需进一步回归。
- 主菜单背景和开场 3D 场景均有严重颜色、光照异常，
  GPU Accurate 与 Sync Memory Operations 没有消除这些异常。
  尚未验证可操作的双人关卡性能或长期稳定性。

日志仍出现 `Geometry streams is not implemented`，对应的图形管线无法编译。
MoltenVK 没有暴露游戏所需的几何流/transform feedback 能力；
这会阻止相关管线创建，但尚未证明它是颜色异常的唯一原因。
已有帧率数据不足以认定完整游戏可玩。

构建和诊断日志保存在 `.cache/diagnostics`；不应把游戏内容、密钥、
存档或程序导出加入源码提交。临时调试器、游戏资源补丁和 Unicorn 测试修改
均已撤回。

## M5 Max 的缓存预算与渲染设置

本机为 40 核 GPU、128 GB 统一内存。Vulkan 经 MoltenVK 1.4.1 转换为 Metal，
运行日志确认设备为 Apple M5 Max。原版集成 GPU 策略为 Conservative 最多 4 GiB、
Aggressive 最多 6 GiB。此分支仅扩大 MoltenVK 集成 GPU 的 Aggressive 预算，
Conservative 和其他 GPU 的原有策略保留。

在 Preferences → Graphics → Advanced 中选择 VRAM Usage Mode = Aggressive，
然后重新启动游戏，使设备与缓存重新初始化。检查日志中的
`GPU cache budget: 16384 MiB (mode: Aggressive, integrated: true)` 即可确认
本机生效；实际预算随驱动启动时报告的可用量缩小。内存预算边界测试通过：
2 个测试、9 条断言，涵盖低预算、用量大于预算和整数上界。

当前独立验证配置使用 2×分辨率、GPU Accurate、16×各向异性过滤、
GPU NVDEC、GPU ASTC 解码、无 ASTC 重压缩；开启异步 GPU 模拟与两层着色器缓存。
为检查 Unreal Engine 渲染正确性，暂时开启 Sync Memory Operations。
这是诊断配置，尚未验证为实际关卡的最优性能配置。
异步呈现、异步着色器编译和 GPU Unswizzle 保持关闭。
Force maximum clocks 的驱动检查不包含 MoltenVK，不能用它提升 Apple GPU 时钟。
Fast GPU Time 改的是模拟 GPU 的计时，也不会给 M5 Max 超频。

增加缓存预算主要减少纹理、缓冲区回收和重复上传；当工作集未达到原预算时，
不能期待明显提升平均帧率。100% 游戏速度下的 30 FPS 不会因为缓存变大而自动
变成 60 FPS；提高分辨率可以增加画面细节，仍需检查真实关卡的帧时间。

第一次 1×基准运行在标题、主菜单和本地新游戏选择页约 30 FPS，
记录的最后 900 个模拟系统帧均值为 33.333 ms、95 分位为 33.445 ms。
主菜单的 3D 背景有明显颜色与光照异常，日志仍有不支持几何流的管线编译错误。
2×、16 GiB 的新构建在主菜单和已观察的开场 3D 镜头也约 30 FPS，
画面异常仍然存在。两次运行还改变了分辨率和其他设置，不能据此量化
缓存扩容的收益；这些数据不能代替实际双人关卡的性能和画面验证。
