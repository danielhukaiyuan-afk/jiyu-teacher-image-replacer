<div align="center">

# ScreenReplace

**极域电子教室 · 画面替换 / 广播控制 工具**

[![Language](https://img.shields.io/badge/Language-C%2B%2B-blue)](https://isocpp.org)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x86-lightgrey)](https://www.microsoft.com/windows)
[![Build](https://img.shields.io/badge/Build-MinGW%20%7C%20MSVC-brightgreen)](#编译)
[![License](https://img.shields.io/badge/License-仅限学习研究-orange)](#免责声明)

让教师端监控本机时看到的是你指定的**图片**；想挡什么功能在选项卡里**自己勾选**；
还带"**取消全屏广播**"按钮——极域广播的全屏本来是退不出去的。

</div>

> ⚠️ **仅供学习研究**，只允许在你拥有或获得授权的设备上使用。
> 不得用于干扰教学秩序、侵犯他人隐私或影响他人设备正常使用。

---

## ✨ 功能特性

| 功能 | 说明 |
|---|---|
| 🖼️ 监控画面 → 图片 | 教师端屏幕监控、截屏、缩略图、"打开看"实时画面全部显示你选的图片（BMP/PNG/JPG），颜色与方向修正**全自动** |
| 📷 自带截屏导入 | 一键截取当前屏幕、保存 PNG 并设为替换图片，不用外部截图工具 |
| 📺 取消全屏广播 | 去掉极域广播窗口的 `CS_NOCLOSE`（关闭按钮从灰变可点）+ 一键最小化 + 拦截全屏/前台抢占 |
| 🙈 忽略广播 | 勾选后教师广播一出现就自动最小化，且无法重新全屏 |
| 💬 挡教师发消息 | 教师端"发消息"弹窗不再出现 |
| ⌨️ 挡教师执行命令 | 教师端远程执行命令（含反弹 shell 类）失效 |
| 📶 在线保持 | 学生端心跳不受影响，教师端始终显示"在线" |
| ✏️ 窗口标题实时修改 | 输入框改标题立即生效并记住，不用关软件 |
| 🔴 生效项目列表 | 右上角红色大字实时显示当前生效的功能 |
| 🚫 关机/重启放行 | 关机/重启指令**不拦截**，正常执行 |

## 🧠 原理速览

**画面替换**（覆盖极域 4 条屏幕捕获链路，结论来自 Dzjs-Trainer 的逆向工程）：

1. `gdi32!BitBlt` —— StudentMain 整屏捕获 / DispcapHelper 全部捕获 → 捕获后把图片画进内存位图；
2. `LibJPEG20!EncodeToJPEGBuffer`（名称或序号 4 导出）—— 缩略图编码前覆盖像素缓冲；
3. `DispFilter.dll` 序号 1 导出 `DispDXGIBitBlt(RECT*, BYTE*)` —— DXGI 帧填充图片；
4. `LibDeskMonitor.dll` 静态导入的 `BitBlt/StretchBlt`（IAT 补丁）—— "打开看"实时画面画成图片。

**阻挡**：

- 消息/命令：StudentMain 是 UDP 指令监听者（默认端口 4705），指令包以 `DMOC`（`44 4D 4F 43`）开头，
  偏移 52 为类型（`0x05` 消息 / `0x0f` 执行命令），钩住 `ws2_32!recvfrom` + `WSARecvFrom` 按类型丢包；
- 广播：识别标题含"广播/演示/共享/屏幕演播室窗口"的窗口（方法来自 JiYuTrainer）：
  - 去掉窗口类样式 `CS_NOCLOSE`（极域用它把关闭按钮变灰）、补 `WS_OVERLAPPEDWINDOW|WS_SYSMENU`；
  - 拦截它对自身的全屏 `SetWindowPos` 与 `SetForegroundWindow` 抢前台。

**通信**：注入器 GUI 与 DLL 通过同目录 `ScreenReplace.ini` 通信，DLL 内看门狗每 400ms 热重载配置，
无需重启任何进程。

## 📁 目录结构

```
.
├── build.bat                     MSVC（Visual Studio 2022）编译脚本
├── build-mingw.bat               MinGW（Dev-C++ 自带 GCC）编译脚本
├── ScreenReplace.zip             打包（编译好的程序 + 全部源码）
├── README.md                     本文件
├── out\                          编译产物
│   ├── ScreenReplace.exe         注入器 GUI（x86，需管理员）
│   └── ScreenReplaceHooks.dll    核心 hook DLL（x86）
└── src\                          源码
    ├── Main.cpp                  界面 / 注入器
    ├── SRHooks.cpp               核心 DLL（画面替换 / 阻挡 / 取消广播）
    ├── include\                  兼容层头文件（sr_compat.h / gcc_shim.h）
    ├── mhook-lib\                内联 API 钩子库（martona/mhook）
    ├── disasm-lib\               反汇编库（mhook 依赖）
    └── res\                      管理员权限与 ComCtl v6 清单
```

> 上传 GitHub 时建议 `.gitignore` 忽略：`out/obj/`、`out/*.exe`、`out/*.dll`、`ScreenReplace.zip`。

## 🔨 编译

### 方式一：Dev-C++ 自带 MinGW（零依赖，实测通过）

```bat
set MINGW=D:\编程\c++\Dev-Cpp\MinGW32
build-mingw.bat
```

脚本会自动定位 MinGW（环境变量 → 注册表 → 常见路径 → PATH）。
> ⚠️ 关键点：MinGW 的 `bin` 目录必须进 PATH，否则 g++ 的子进程 `cc1plus.exe` 会因找不到
> 运行时 DLL 静默失败（退出码 `0xC0000135`，零报错）。

### 方式二：Visual Studio 2022

安装 VS2022 并勾选"使用 C++ 的桌面开发"，然后：

```bat
build.bat
```

产物均在 `out\`：`ScreenReplace.exe` + `ScreenReplaceHooks.dll`（**必须放同一目录**）。
本项目为 **x86** 构建（极域 2016 学生端是 32 位进程）；若遇到 64 位学生端，把编译参数改成 x64 重编。

## 🚀 使用

1. **以管理员身份**运行 `ScreenReplace.exe`；
2. 「基本」选项卡：选图（或点 **截屏并导入**）→ 勾"启用图片替换"；
3. 「阻挡选项」选项卡：勾选要挡的功能（忽略广播 / 挡消息 / 挡命令），需要时点 **取消全屏广播**；
4. 点 **保存配置并注入**；程序每 2 秒自动补注入（学生端重启、DispcapHelper 后启动都自动跟上）；
5. 不用时点 **卸载并还原**：只写关闭配置，DLL 留在进程内纯透传（不远程卸载，避免还原钩子竞态
   把 StudentMain 弄死），学生端下次重启后 DLL 自然消失。

配置保存在程序同目录 `ScreenReplace.ini`（DLL 每 400ms 热重载）：

```ini
[ScreenReplace]
ImagePath=C:\fake\desktop.png      ; 替换图片
EnableImage=1                      ; 图片替换开关
BlockBroadcast=1                   ; 忽略广播
BlockMsg=1                         ; 挡教师发消息
BlockCmd=1                         ; 挡教师执行命令
LogLevel=1

[UI]
Title=ScreenReplace v4             ; 窗口标题（可在程序里改）
```

调试日志：`%TEMP%\ScreenReplace.log`（GUI 里点"打开日志"）。

## ❓ 疑难排查

- **教师端还是能看到真实屏幕**：确认 exe 与 dll 是同一批解压的；确认勾了"启用图片替换"并点了
  "保存配置并注入"；看日志有没有 `已注入 StudentMain.exe` / `已注入 DispcapHelper.exe`。
- **画面颜色/方向不对**：v2+ 已固定 BGR 交换 + 垂直翻转；若换极域版本又出问题，改
  `SRHooks.cpp` 顶部的 `kSwapRB` / `kFlipV` 常量重编。
- **广播窗口没被识别**：不同版本标题不同，把 `SRHooks.cpp` 里 `IsBroadcastTitle()` 加关键词重编。
- **注入失败**：确认管理员运行；学生端以 SYSTEM 运行时需同等权限运行注入器。
- **冻结后教师端显示离线**：你的版本教师端会主动 ping 学生端，这种环境只勾消息/命令阻挡。

## 🙏 致谢

本项目整合了以下公开项目的思路与代码：

| 项目 | 用途 |
|---|---|
| [imengyu/JiYuTrainer](https://github.com/imengyu/JiYuTrainer) | 广播窗口化（CS_NOCLOSE 移除、全屏/前台拦截） |
| [yunsjxh/Dzjs-Trainer](https://github.com/yunsjxh/Dzjs-Trainer) | 画面替换四链路逆向结论 |
| [martona/mhook](https://github.com/martona/mhook) | 内联 API 钩子库（请遵循其许可证） |
| Jiyu_udp_attack | 指令包结构（DMOC 魔数 / 类型偏移 52） |

## 📜 免责声明

本工具仅用于**学习研究**和**你拥有/获授权**的设备。禁止用于干扰教学秩序或影响他人设备，
由此产生的一切后果由使用者自行承担。
