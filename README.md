# Context Reader / 语境阅读

Context Reader 是一个以 PDF 语境阅读、文本高亮和 Markdown 笔记为核心的跨端阅读系统。首发产品为 Electron 桌面客户端，其他宿主在核心阅读闭环稳定后按需引入。

本项目采用 **Kernel First** 架构：业务能力只在 C++ Reader Kernel 中实现一次，Electron、原生客户端、CLI、插件和可选服务端通过不同 Binding/Host 复用同一个内核。

## 当前状态

当前仓库处于架构设计阶段，尚未进入功能实现。V0.1 文档用于建立模块边界、运行形态和开发约束，后续重要调整通过 ADR 记录。

## 核心决策

- C++ Reader Kernel 是唯一业务实现，但不是全局单例。
- Kernel 通过稳定 Application Facade 提供用例，不感知 Electron、HTTP 或具体 UI。
- Electron 首发调用链为 Renderer typed IPC -> Utility Process -> Node-API -> C++ Application Facade；不经过 C ABI。
- 首发平台为 Windows x64；Pixi 锁定开发工具环境，CMake 3.28+ 编排构建与源码依赖，不引入 vcpkg 或 Conan。
- C ABI 是后续原生客户端和跨语言嵌入的稳定边界，与 Electron Binding 并列适配同一 Application Facade。
- HTTP/readerd 是浏览器、远程访问和多客户端场景的可选宿主，不是本地主路径。
- MuPDF、SQLite 和文件系统位于适配器层，类型不能泄漏到领域层和公共 ABI。
- 编程风格以组合优先，继承仅用于少量稳定的 Port 接口。
- PDF 原文件不可变；高亮和笔记保存在工作区，不随编辑直接修改 PDF。
- Fixture、Probe、Replay、Bench 和 ABI Check 构成独立的工程控制面，使复杂原生行为可观察、可重放和可验证。
- 基础设施工具优先使用正式 Application Facade 或已经实现的 Binding；通用库只从已经重复出现的真实需求中提炼。

## 文档

- [架构设计 V0.1](docs/architecture-v0.1.md)
- [编程指导 V0.1](docs/programming-guide-v0.1.md)
- [基础设施与验证策略 V0.1](docs/infra-strategy-v0.1.md)
- [实施计划 V0.1](docs/implementation-plan-v0.1.md)
- [ADR-0001：Electron 首发与 Node-API 调用边界](docs/adr/0001-electron-first-node-binding.md)
- [ADR-0002：Windows x64、Pixi 工具环境与 CMake 依赖管理](docs/adr/0002-windows-cmake-dependencies.md)
- [ADR-0003：P0 工具链验收与 MuPDF 商业许可](docs/adr/0003-p0-toolchain-validation.md)

## 开发与验证

开发机需要安装 Pixi 和包含 UCRT64 GCC 的独立 MSYS2。项目通过 Pixi activation 自动定位 Scoop MSYS2，也可以显式设置 `CONTEXT_READER_MSYS2_ROOT`。

```powershell
pixi install --frozen
pixi run p0
```

`pixi run p0` 会构建并测试 `reader_core`、重新安装锁定的 npm 依赖、编译 `reader_node`、检查 PE 导入边界，并由 Electron Utility Process 加载原生模块。

若代理只配置在 Windows 用户设置，首次下载 Electron 或 CMake.js headers 前需要为当前 shell 设置 `HTTP_PROXY`、`HTTPS_PROXY` 和 `ELECTRON_GET_USE_PROXY=1`；代理地址不得写入仓库。

## 计划中的主要制品

```text
reader_core          C++ 领域与应用内核
reader_node          Electron/Node-API 首发绑定
reader_c             后续原生/跨语言嵌入使用的稳定 C ABI
reader_cli           后续命令行宿主
reader_plugin_host   后续插件运行时
readerd              后续可选的本地或远程服务宿主

reader-probe         PDF、文本布局、坐标和 Anchor 诊断
reader-fixture       回归语料与预期结果管理
reader-replay        Facade/Binding 调用记录与重放
reader-bench         可重复的性能基准与趋势比较
reader-abi-check     C ABI 生命周期与兼容性验证
reader-workspace     工作区检查、迁移和离线修复
```
