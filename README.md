# 窗口尺寸固定 · Window Size Lock

把目标窗口调整到喜欢的大小，按住蓝色按钮拖到窗口上，松开即可固定。可以连续添加多个窗口，为每个窗口设定不同的宽、高，再单独或批量启用、暂停。记录自动保存，窗口关闭或工具退出后仍保留，重新打开匹配的窗口后自动连接。

**Windows 10 / 11 · 原生 C++ / Win32 · 单个 EXE · 无需安装 · MIT 开源**

[下载最新便携版](https://github.com/hujianj/window-size-lock/releases/latest) · [验证记录](docs/VALIDATION.md) · [更新记录](CHANGELOG.md)

![v1.2.0 界面示意，窗口名和尺寸为模拟数据](docs/ui-preview.svg)

上图是界面示意，使用模拟窗口名称和尺寸。实际程序已在 Windows 11 的两个经典记事本窗口上完成交互验证。

## 选取与固定

1. 下载 Release 中的 `WindowSizeLock.exe`，或下载 ZIP 并解压，双击运行。
2. 把目标窗口调整到需要的尺寸。
3. 按住蓝色按钮，拖到目标窗口后松开。窗口会追加到“固定管理”，自动记录当时的外部宽、高。

也可以双击“可选窗口”的一行，或按住 Ctrl / Shift 选择多行后点击“加入固定”。加入新窗口会保留其他目标；重复加入已有目标不会覆盖它的设定或产生重复记录。最多管理 **32 个窗口**，本版实测了两个同时管理的窗口。

窗口可以自由移动。拖动过程中允许暂时改变尺寸；松开后等待 Windows 移动、缩放与吸附状态稳定再恢复：连续稳定至少 300 ms，移动或缩放结束另留 400 ms 缓冲。本机的一次缩放实测约 500 ms 后恢复，这不是所有电脑上的响应保证。最小化、隐藏或手动暂停时停止纠正，恢复显示并启用后继续固定。

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
- “退出”或右上角关闭：停止本次固定并退出，保留所有记录。再次运行工具后自动连接匹配的窗口。
- “刷新列表”：更新可选窗口。关闭的目标留在“固定管理”中等待重新打开。

## 自动保存与重新打开

从 **v1.3.0** 起，加入窗口、应用尺寸、启用、暂停和移除都会自动保存，无需点击保存按钮。

1. 关闭目标窗口后，管理列表显示“等待窗口”，仍保留尺寸；暂停的记录显示“暂停·等待”。
2. 重新打开相同窗口，工具按程序完整路径、窗口类型和窗口标题重新识别。唯一匹配时自动连接，启用的记录恢复设定尺寸，暂停的记录继续暂停。
3. 退出并重新运行固定工具也会载入所有记录。可以先运行工具再打开目标，也可以先打开目标再运行工具。
4. 窗口尚未打开时，也能在管理列表选择记录，修改宽、高并点击“应用尺寸”，或改变启用/暂停状态。
5. 要永久取消记录，请点击“移除所选”。托盘“解除全部固定”删除全部记录；普通退出保留记录。

例如固定 `配置.txt - 记事本` 为 `950 × 700`，关闭并重新打开它后自动恢复；`另一个.txt - 记事本` 可以保留独立的 `900 × 650`。经典记事本标题开头表示未保存修改的 `*` 不影响匹配。

配置保存在当前 Windows 用户的 **`%LOCALAPPDATA%\WindowSizeLock\settings.dat`**。更换 EXE 所在文件夹或升级到新版不需要迁移配置；换电脑或 Windows 用户不会自动同步。关闭工具后可备份此文件。旧版没有保存功能，升级首次需要重新选取一次，无法恢复旧版已丢失的记录。工具本身不会自动开机运行，登录后需要启动它。

匹配以窗口标题为准，不读取文档内容。标题改变时会更新当前已连接记录；重新打开时标题不同，需要重新选取。若多个窗口或记录的程序路径、类型、标题完全相同，不会任意自动选择；可手动选取目标，重复记录需先移除。不同目录的同名记事本文档若显示相同标题，工具无法区分其文档路径，建议使用不同文件名。程序路径变化、读取权限不足或没有标题也可能需要重新选取。

配置使用先写临时文件再替换的方式，并校验版本、数量、尺寸和字符串。文件损坏或版本不支持时提示且拒绝载入；直接退出保留原文件，重新加入或修改设置会写入新配置。写入失败会提示并自动重试。

## 最大化与屏幕尺寸

右侧分别显示目标所在屏幕的分辨率、扣除任务栏后的最大化工作区、窗口实际外部尺寸以及 DWM 可见外框。Windows 的外部矩形包含隐形调整边框，数字可能比肉眼看到的窗口略大。

本机 100% 缩放的实际例子：

| 项目 | 尺寸 |
| --- | --- |
| 屏幕分辨率 | 2358 × 1432 px |
| 最大化工作区 | 2358 × 1384 px |
| 经典记事本最大化后的外部尺寸 | 2374 × 1400 px |
| 经典记事本最大化后的可见外框 | 2358 × 1384 px |

要固定成某个窗口真正的最大化尺寸，选中它后点击 **“使用此窗口的最大化尺寸”**。程序会最大化这个窗口、读取它的实际外部尺寸并固定。也可以先最大化目标，再拖动选取或从列表加入。

v1.2.1 针对“尺寸已等于最大化尺寸，再次最大化却没有正确铺满”的反馈加强处理。读取最大化尺寸前，先等待窗口矩形和最小化/最大化状态连续稳定，并核对可见外框是否覆盖工作区。记录的最大化尺寸被视为屏幕尺寸模式，避免动画、边框或 DPI 的临时差异导致错误还原。手动输入工作区、屏幕尺寸或带系统隐形边框的尺寸，也允许系统最大化。

尺寸相同不代表位置正确。如果窗口报告已最大化，但可见范围没有覆盖工作区，程序会先请求还原，等待稳定后再请求最大化，最多修复两次；目标不响应或持续限制位置时，暂停该目标并提示。普通状态的屏幕尺寸窗口移动到顶部时，另有请求最大化的兜底。固定普通小尺寸时，最大化仍会恢复到设定宽、高。

v1.2.1 在 Windows 11 经典记事本上通过了点击最大化、顶部吸附、手动工作区尺寸、屏幕尺寸与最大化外部尺寸的历史验证，详见 [v1.2.1 记录](docs/VALIDATION-1.2.1.md)。v1.3.0 使用上表的当前显示配置，验证了实际最大化尺寸和屏幕尺寸模式在工具重启后保留，再次系统最大化时可见外框正确覆盖工作区。反馈中的旧版异常未在本机稳定复现；异常位置修复和顶部兜底分支未通过故障注入触发，不能把结果解释为所有程序和显示环境均已验证。详见[本版验证记录](docs/VALIDATION.md)。

## 轻量实现与资源

界面使用 Windows 原生控件与 GDI，没有 Electron、WebView、.NET 或 Python 运行依赖。静态 C++ 运行库，v1.3.0 x64 EXE 为 **182,272 字节（178 KiB）**。只有存在等待记录时，才每秒枚举一次窗口寻找匹配；全部连接后不执行重新发现。配置只在设置改变时写入。

通过 WinEvent 事件通知恢复尺寸，配合 1 秒检查兜底及待稳定时的 80 ms 检查；常规纠正尝试间隔约 60 ms，持续冲突超过 3 秒后降至约 500 ms。旧版 v1.2.0 后台收起、两个目标启用且静止的本机 20 秒采样：专用内存约 **3.00 MiB**，工作集约 **19.73 MiB**，CPU 时间增量约 **0.234 秒**，折合一个逻辑处理器约 **1.17%**。这是历史样本，v1.3.0 未重新进行相同条件的资源采样，不能作为新版测量值或所有电脑的表现保证。详见[旧版记录](docs/VALIDATION-1.2.0.md)。

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

正常运行会在上述用户本地目录保存窗口标题、程序路径、窗口类型、尺寸与启用状态，不读取目标文档内容，不联网，不修改目标程序文件，不安装驱动或服务。配置没有加密，分享或备份时请留意本地标题和路径。正常使用不创建诊断日志；源码保留以 `.diagnostics` 标记启用本地验证日志的开发入口，发布包不包含配置、该标记或原始日志。

本项目为独立编写的实现，采用 [MIT License](LICENSE)。

## English

A tiny, portable native Windows utility that captures and restores the outer size of selected windows. Manage up to 32 targets with independent sizes and selective or batch enable, pause and removal. Drag the blue picker onto a window, use the multi-select list, or press `Ctrl + Alt + Z`. Edit width and height and apply to the window shown in the detail pane. `Ctrl + Alt + U` removes only the foreground target. Moving is allowed; minimizing pauses enforcement. Version 1.2.1 waits for stable geometry and state, preserves screen-sized maximization and verifies visible work-area coverage with bounded placement repair. Maximize and top snap passed classic Notepad tests, but the reported old-version failure was not reliably reproduced and repair fallback branches were not fault-injection tested. The panel shows monitor resolution, work area, outer size and visible frame bounds. No external runtime or installation required. Windows 11 x64 tested with two targets; Windows 10 and mixed-DPI displays have not been physically tested.

Version 1.3.0 persists records in `%LOCALAPPDATA%\WindowSizeLock\settings.dat`. Closing a target or exiting the utility preserves dimensions, enabled/paused state and screen-size mode. Reconnection requires a unique match of executable path, window class and normalized title. Offline records can be edited and paused. Removal deletes the saved record. Titles and paths are stored locally without encryption; document contents are not read. Same-title documents cannot be distinguished by document path. The final 178 KiB executable passed Windows 11 close/reopen, utility restart, offline editing, paused reattachment, two-record independence and corrupt-file rejection tests.
