# Context Reader / 语境阅读

Context Reader 是一个以 PDF 语境阅读、文本高亮和 Markdown 笔记为核心的跨端阅读系统。首发产品为 Electron 桌面客户端，其他宿主在核心阅读闭环稳定后按需引入。

本项目采用 **Kernel First** 架构：业务能力只在 C++ Reader Kernel 中实现一次，Electron、原生客户端、CLI、插件和可选服务端通过不同 Binding/Host 复用同一个内核。

## 当前状态

当前仓库已完成 P0 工具链、P1 PDF 风险验证与 P2 Electron 纵向闭环，并已开始 P3 数据与边界稳定。首批 P3 能力包括 Electron Main 单实例限制、Workspace 独占写入与崩溃释放、明确的 `WORKSPACE_BUSY`、孤立 PDF 对象检查/清理、schema v1 -> v2 迁移前 SQLite 备份，以及 `reader-workspace inspect/verify/migrate --dry-run`。Node-API 完整契约、Utility Process 重启竞态和真实 Replay 仍在后续 P3 工作中。

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
- [第三方依赖清单 V0.1](docs/dependencies-v0.1.md)
- [ADR-0001：Electron 首发与 Node-API 调用边界](docs/adr/0001-electron-first-node-binding.md)
- [ADR-0002：Windows x64、Pixi 工具环境与 CMake 依赖管理](docs/adr/0002-windows-cmake-dependencies.md)
- [ADR-0003：P0 工具链验收与 MuPDF 商业许可](docs/adr/0003-p0-toolchain-validation.md)
- [ADR-0004：Clang 优先的 UCRT64 工具链](docs/adr/0004-clang-first-ucrt64-toolchain.md)
- [ADR-0005：SQLite Workspace v1 与内容寻址对象仓库](docs/adr/0005-sqlite-workspace-v1.md)
- [ADR-0006：Annotation、Quote Anchor 与 Note Workspace v2](docs/adr/0006-annotation-note-workspace-v2.md)

## 开发与验证

开发机需要安装 Pixi 和包含 UCRT64 Clang（首选）或 GCC（回退）的独立 MSYS2。项目通过 Pixi activation 自动定位 Scoop MSYS2，也可以显式设置 `CONTEXT_READER_MSYS2_ROOT`；设置 `CONTEXT_READER_COMPILER=clang|gcc` 可以覆盖自动选择。

```powershell
pixi install --frozen
pixi run p0
pixi run p1
pixi run p2
npm run start:p2
pixi run workspace inspect <workspace-path>
pixi run workspace verify <workspace-path>
pixi run workspace migrate <workspace-path> --dry-run
pixi run p3-single-instance-smoke
```

`pixi run p0` 会构建并测试 `reader_core`、重新安装锁定的 npm 依赖、编译 `reader_node`、检查 PE 导入边界，并由 Electron Utility Process 加载原生模块。

`pixi run p1` 会从官方 tag 准备锁定提交的 MuPDF 1.28.3 最小静态构建，验证 Fixture manifest，并运行 PDF Port、坐标、真实 MuPDF Adapter 和 `reader-probe inspect` 契约测试。独立 MSYS2 还需要安装 `make` 和 UCRT64 `pkgconf`；MuPDF 源码及构建产物只保存在忽略的 `build/` 目录。

`pixi run p2` 在 P1 基础上构建 SQLite Workspace 与 P2 `reader_node`，运行持久化/重启集成测试、PE 导入边界检查，并验证 Utility Process 及沙箱化 Renderer 两条真实 Electron 路径。Utility smoke 还会验证排队 Job 取消，以及导入事务提交前后终止进程后的恢复结果；Renderer smoke 会完成创建、导入、渲染、文本选择、高亮和笔记保存，并在 `build/renderer-p2-smoke.png` 留下非空像素检查通过的截图。

`npm run start:p2` 启动当前 P2 桌面宿主。Renderer 只能使用 preload 暴露的限定 API；工作区和 PDF 路径必须先由 Main 文件对话框授权，原生模块、SQLite、MuPDF 和任意文件系统访问均不暴露给 Renderer。

产品不支持同时运行两个 Context Reader 应用实例。Electron Main 启动时取得应用单实例锁；再次启动时，新实例立即退出，并由已有实例恢复和聚焦主窗口。`pixi run p3-single-instance-smoke` 会用隔离的用户数据目录启动两个真实 Electron 进程验证该行为。Kernel 的 `workspace.lock` 是独立的数据安全边界，只用于防御 Utility Process 重启短暂重叠、维护工具与应用冲突或未来其他宿主误用同一 Workspace；它不表示产品支持两个应用并发编辑。

`reader-workspace` 的首个 P3 版本输出单行 JSON。所有命令遵守 Workspace 独占锁；`migrate --dry-run` 只报告当前与目标 schema，不修改数据库。`verify` 会报告 SQLite、引用对象与孤立对象问题；孤立对象清理由 Kernel 显式 API 提供，当前 CLI 不自动删除数据。

```powershell
pixi run probe inspect tests\corpus\generated\basic-rotated-cropbox.pdf --output build\probe-output
pixi run probe render tests\corpus\generated\basic-rotated-cropbox.pdf --page 1 --scale 2 --output build\probe-render
pixi run probe text tests\corpus\generated\basic-rotated-cropbox.pdf --page 1 --output build\probe-text
pixi run probe roundtrip tests\corpus\generated\basic-rotated-cropbox.pdf --page 1 --scale 1.5 --dpr 2 --output build\probe-roundtrip
pixi run probe compare build\probe-actual build\probe-baseline
pixi run fixture-verify
```

Probe 将结构化结果写到标准输出；指定 `--output` 时还会生成已脱敏输入路径的 `run-manifest.json`，以及命令对应的页面信息、PNG、文本布局或坐标 JSONL 产物。`render` 必须指定输出目录。`compare` 输出新增、缺失和内容哈希变化，发现差异时返回退出码 `6`，且不提供批量接受操作。

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
