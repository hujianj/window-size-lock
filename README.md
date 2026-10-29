# 窗口尺寸固定 · Window Size Lock

把目标窗口调整到喜欢的大小，按住蓝色按钮拖到窗口上，松开即可固定。可以连续添加多个窗口，为每个窗口设定不同的宽、高，再单独或批量启用、暂停。

**Windows 10 / 11 · 原生 C++ / Win32 · 单个 EXE · 无需安装 · MIT 开源**

[下载最新便携版](https://github.com/hujianj/window-size-lock/releases/latest) · [验证记录](docs/VALIDATION.md) · [更新记录](CHANGELOG.md)

![v1.2.0 界面示意，窗口名和尺寸为模拟数据](docs/ui-preview.svg)

上图是界面示意，使用模拟窗口名称和尺寸。实际程序已在 Windows 11 的两个经典记事本窗口上完成交互验证。

## 选取与固定

1. 下载 Release 中的 `WindowSizeLock.exe`，或下载 ZIP 并解压，双击运行。
2. 把目标窗口调整到需要的尺寸。
3. 按住蓝色按钮，拖到目标窗口后松开。窗口会追加到“固定管理”，自动记录当时的外部宽、高。

也可以双击“可选窗口”的一行，或按住 Ctrl / Shift 选择多行后点击“加入固定”。加入新窗口会保留其他目标；重复加入已有目标不会覆盖它的设定或产生重复记录。最多管理 **32 个窗口**，本版实测了两个同时管理的窗口。

窗口可以自由移动。拖动过程中允许暂时改变尺寸；松开后等待 Windows 移动、缩放与吸附状态稳定，约 200 ms 后检查并恢复。最小化、隐藏或手动暂停时停止纠正，恢复显示并启用后继续固定。

## 手动修改宽、高

1. 在“可选窗口”或“固定管理”中单击要修改的窗口，右侧显示其详情并读取尺寸。
2. 修改“外部宽度”和“外部高度”，例如从 `1000 × 700` 改成 `900 × 650`，或 `1100 × 750`。
3. 点击“应用尺寸”。只修改右侧当前查看的窗口，并启用该窗口的固定；其他窗口保留原设定。

输入值是 **1–32767 的整数物理像素**。输入过程中不会被后台刷新覆盖；不合法的值不会替换原设定。目标程序可能有自己的最小尺寸或布局约束，不能保证接受所有输入值。

## 只固定其中一个或两个

在“固定管理”中用 Ctrl / Shift 多选，再点击“启用所选”“暂停所选”或“移除所选”。“全部启用”和“全部暂停”作用于所有记录。暂停会保留尺寸记录；移除会解除该窗口，其他窗口继续固定。

- `Ctrl + Alt + Z`：加入当前前台窗口。
- `Ctrl + Alt + U`：仅解除当前前台窗口，其他目标继续固定。
- “收起到托盘”：后台继续固定。点击托盘图标或再次运行 EXE，唤回同一面板。
- 托盘菜单“解除全部固定”：解除所有目标。
- “退出”或右上角关闭：解除所有目标并退出。
- “刷新列表”：更新可选窗口。关闭的目标会自动移除，其他目标保留。

程序退出后不保留记录，下次运行需要重新选取。

## 最大化与屏幕尺寸

右侧分别显示目标所在屏幕的分辨率、扣除任务栏后的最大化工作区、窗口实际外部尺寸以及 DWM 可见外框。Windows 的外部矩形包含隐形调整边框，数字可能比肉眼看到的窗口略大。

本机 100% 缩放的实际例子：

| 项目 | 尺寸 |
| --- | --- |
| 屏幕分辨率 | 2344 × 1432 px |
| 最大化工作区 | 2344 × 1384 px |
| 经典记事本最大化后的外部尺寸 | 2360 × 1400 px |
| 经典记事本最大化后的可见外框 | 2344 × 1384 px |

要固定成某个窗口真正的最大化尺寸，选中它后点击 **“使用此窗口的最大化尺寸”**。程序会最大化这个窗口、读取它的实际外部尺寸并固定。也可以先最大化目标，再拖动选取或从列表加入。

v1.2.0 修复了旧版“只要检测到最大化，就强制还原”的问题。当实际最大化尺寸匹配设定时，保留最大化状态；手动输入正好等于该屏幕分辨率或工作区的尺寸时，也允许系统最大化边框的差异。点击最大化按钮、拖到屏幕顶部都可以正常生效。固定普通小尺寸时，最大化仍会恢复到设定宽、高。

## 轻量实现与资源

界面使用 Windows 原生控件与 GDI，没有 Electron、WebView、.NET 或 Python 运行依赖。静态 C++ 运行库，v1.2.0 x64 EXE 为 **172,032 字节（168 KiB）**。

通过 WinEvent 事件通知恢复尺寸，配合 1 秒检查兜底；常规纠正尝试间隔约 60 ms，持续冲突超过 3 秒后降至约 500 ms。后台收起、两个目标启用且静止的本机 20 秒采样：专用内存约 **3.00 MiB**，工作集约 **19.73 MiB**，CPU 时间增量约 **0.234 秒**，折合一个逻辑处理器约 **1.17%**。这些是具体样本，不代表所有电脑或持续拖动状态，详见[验证记录](docs/VALIDATION.md)。

## 兼容范围

- 面向 Windows 10 / 11，提供 x64 便携版。当前真机实测为 Windows 11 IoT Enterprise LTSC x64 10.0.26100；**Windows 10 尚未真机验证**。
- 固定的是 `GetWindowRect` 返回的外部尺寸，单位为物理像素，包含窗口边框。
- 支持允许外部改变尺寸的普通桌面窗口。目标程序的尺寸下限、自绘布局、独占全屏或受保护窗口可能限制固定效果。
- 权限需要与目标匹配。普通权限进程通常无法控制管理员权限目标，必要时以相同权限运行并重新选取。
- 多屏不同 DPI、跨屏移动、任务栏位置或工作区变化尚未逐项实测，可能触发目标自己的布局规则。
- 快捷键被占用时界面会提示，可以继续用拖动或列表。
- 32 是管理容量，本次没有进行 32 窗口压力测试。

## 从源码构建

安装 Visual Studio 2022 / Build Tools，勾选“使用 C++ 的桌面开发”和 Windows 10/11 SDK，在仓库根目录运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\src\build.ps1
```

生成文件位于 `src/bin/WindowSizeLock.exe`。脚本使用 MSVC `/O1 /MT /W4` 编译，默认 x64；可用 `-OutDir` 指定输出目录，用 `-Architecture x86` 选择架构。公开发布包为已实测的 x64 构建。GitHub Actions 编译并上传产物；CI 编译成功不等于桌面交互验证。

## 数据与许可

正常运行不创建配置或日志，不读取目标文档内容，不联网，不修改目标程序文件，不安装驱动或服务。窗口标题与尺寸仅用于本地显示。源码保留以 `.diagnostics` 标记启用本地验证日志的开发入口，发布包不包含该标记或原始日志。

本项目为独立编写的实现，采用 [MIT License](LICENSE)。

## English

A tiny, portable native Windows utility that captures and restores the outer size of selected windows. Manage up to 32 targets with independent sizes and selective or batch enable, pause and removal. Drag the blue picker onto a window, use the multi-select list, or press `Ctrl + Alt + Z`. Edit width and height and apply to the window shown in the detail pane. `Ctrl + Alt + U` removes only the foreground target. Moving is allowed; minimizing pauses enforcement. Matching maximized dimensions now remain maximized, including top-edge snap. The panel shows monitor resolution, work area, outer size and visible frame bounds. No external runtime or installation required. Windows 11 x64 tested with two targets; Windows 10 is intended but has not yet been tested on a physical system.
