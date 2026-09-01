# Context Reader 实施计划 V0.1

状态：Draft

首发宿主：Electron

首发平台：Windows x64

核心语言：C++20

目标：以机器可验证的阶段交付产品 v0.1，而不是一次铺开所有宿主和工程工具

## 1. 已确定的交付基线

首发调用链：

```text
Electron Renderer
       -> typed IPC
Electron Utility Process
       -> Node-API (reader_node)
C++ Application Facade
       -> reader_core
```

`reader_node` 与 `reader_core` 使用同一工具链构建和发布，直接调用 C++ Application Facade。C ABI 不进入这条路径；它在出现原生或其他语言 FFI 消费者后作为并列 Binding 实现。

所有 C++ 目标统一以 C++20 编译。已有只使用 C++17 特性的代码可以直接纳入，但不为不同业务模块配置不同的语言标准。

Pixi 锁定 CMake、Ninja 等开发工具版本并提供统一任务入口。构建系统使用 CMake 3.28+；源码依赖获取和构建由 CMake 统一编排：优先使用 `FetchContent` 和 `add_subdirectory`，不提供原生 CMake 构建的依赖使用受控 `ExternalProject`。不引入 vcpkg 或 Conan，所有依赖必须锁定版本、来源和完整性哈希。Pixi/conda 包不作为 MSYS2 UCRT64 工具链的链接时 C++ 库。

## 2. 产品 v0.1 范围

包含：

- 创建或打开单用户 Workspace。
- 导入本地 PDF，保存不可变原文件并建立稳定 Document/Version ID。
- 页面渲染、滚动、缩放、文本布局和文本选择。
- 创建、更新和删除高亮；高亮使用 PDF 页面坐标和 Quote Anchor。
- Markdown 源码编辑加预览、自动保存、revision 冲突检测。
- 行内/块级数学公式和图片附件。
- PDF 文本与笔记搜索。
- 重启恢复、导出备份和基础工作区验证。
- Electron 安装包，以及首发环境上的正确性和性能验证。

不包含：

- 原生客户端、Web、远程访问和多人协作。
- 产品级 CLI、第三方插件和 ContextGraph。
- OCR 和扫描 PDF 文本识别；扫描 PDF 只支持页面渲染。
- C ABI；除非产品 v0.1 开发期间出现已经确认的 FFI 消费者。
- GPU 纹理共享、复杂共享内存和跨进程通用 RPC 框架。

## 3. 阶段依赖

```text
P0 决策与骨架
  -> P1 PDF 风险验证
      -> P2 Electron 纵向闭环
          -> P3 数据与边界稳定
              -> P4 产品 v0.1 发布
                  -> P5 后续宿主与扩展
```

阶段可以在内部并行，但不能用后续功能数量代替前一阶段的退出条件。

## 4. P0：决策与工程骨架

本地实施状态：2026-08-29 已完成；`pixi run p0` 验证通过。GitHub Actions 工作流已建立，首次推送后的远程 CI 结果仍需归档。

交付内容：

- 以 Windows x64 为首发目标；MSYS2 UCRT64 GCC 16.2.0 已完成初始验证，ADR-0004 将 UCRT64 Clang 22.1.8 调整为首选并保留 GCC 回退。
- MuPDF 商业许可已经取得；PDF Fixture 仍按来源、许可和可再分发状态逐项登记。
- 建立 `pixi.toml`/`pixi.lock`，锁定 CMake、Ninja 等开发工具；建立 CMake 依赖清单，通过 `FetchContent`、`add_subdirectory` 或受控 `ExternalProject` 锁定源码依赖版本、来源和完整性哈希。
- 建立 CMake target 骨架、`Result/Error`、稳定 ID、Composition Root 和最小测试入口。
- 建立只包含必要检查的 CI；每个目标环境分别构建一次并复用该产物进行测试。

退出条件：

- 编译器、Node/Electron 版本和 MuPDF 许可已经记录为 ADR 或决策记录，不存在阻止 P1 集成 MuPDF 的许可和工具链问题。
- 空 Kernel、一个单元测试和一个 Electron 原生模块加载测试能够在首发环境构建运行。
- 系统 CMake 必须能通过 preset 确定性选择 UCRT64 Clang 或 GCC，且生成的 `.node` 模块能被目标 Electron 版本加载；不能仅以命令行编译成功作为工具链验收。
- CMake target 依赖方向可以被 CI 检查。

## 5. P1：PDF 风险验证

本地实施状态：2026-08-29 已完成；`pixi run p1` 验证通过。MuPDF 1.28.3 按官方 tag 提交锁定，Clang 最小静态构建、CMake imported target、`PdfEngine` Adapter 和串行 `DocumentSession` 已通过真实集成测试。`reader-probe inspect/render/text/roundtrip/compare` 输出版本化页面信息、PNG、文本布局、坐标 JSONL、run manifest 和结构化基线差异。Fixture manifest 覆盖正常、CJK、双栏、旋转、CropBox、损坏和扫描场景，并校验授权元数据、路径和 SHA-256。

交付内容：

- `PdfEngine` Port、MuPDF Adapter 和串行拥有文档句柄的最小 `DocumentSession`。
- `reader-probe inspect/render/text/roundtrip`，不提前建设通用工具框架。
- 正常、CJK、双栏、旋转、CropBox、损坏和扫描 PDF 的最小 Fixture 集合。
- 页面坐标、设备像素坐标、缩放和 DPR 的显式值类型与变换测试。

退出条件：

- 固定 Fixture 可以稳定输出页面信息、PNG、文本布局和版本化诊断数据。
- 正向/逆向坐标变换在已记录容差内往返，高亮覆盖层与页面渲染使用同一变换定义。
- 损坏和扫描 PDF 返回稳定的受支持结果或错误，不导致测试进程无说明退出。
- 基线变更能够显示结构化差异，不能无审查批量接受。

## 6. P2：Electron 首个纵向闭环

本地实施状态：已完成。Workspace schema v2、Quote Anchor 高亮、revision 化纯文本 Markdown Note，以及沙箱化 Renderer -> 限定 preload/IPC -> Utility Process -> Node-API -> Facade 的阅读、DOM 选择、高亮、笔记自动保存和重启恢复路径均已通过测试。Renderer 验收使用同一 Workspace 连续启动两个真实 Electron 进程，并在首次显式关闭后由第二次启动核对持久化状态。导入、打开和渲染使用带稳定 ID 与取消令牌的 Job；Utility Process 在导入事务 `COMMIT` 前后被终止后，工作区可重新打开并分别恢复为未提交与已提交状态，且只读不变量检查有效。

交付内容：

- 最小 Workspace、SQLite migration 和内容寻址对象仓库。
- Document 导入/打开、页面渲染、文本选择、Annotation 和纯文本 Markdown Note。
- Electron Renderer、typed IPC、Utility Process、`reader_node` 与 Application Facade 的完整路径。
- 最小工作区只读验证逻辑，先作为测试/库能力存在，不要求立即形成完整 `reader-workspace` 工具。

退出条件：

- 端到端测试可以完成“导入 -> 阅读 -> 选择 -> 高亮 -> 写笔记 -> 关闭 -> 重启恢复”。
- Renderer 不加载原生模块，不直接访问文件系统、SQLite 或 MuPDF。
- 导入、打开和渲染以可取消 Job 运行；Node 主线程与 Renderer 不被长任务阻塞。
- 数据库提交前后终止 Utility Process，工作区仍可再次打开并通过不变量检查。

## 7. P3：数据与边界稳定

本地实施状态：已完成。Electron Main 使用单实例锁；Workspace 提供独占写入、稳定 Busy 错误、孤立对象恢复和迁移前备份；导入、Note 自动保存和迁移具有提交前后故障验证。Utility Process 由带 generation 的 Main supervisor 自动重启并恢复 Workspace/Document 意图；验收会在 Note 提交后、Document 关闭后和 Workspace 关闭后丢弃响应并终止 Utility，分别验证权威 revision、陈旧 revision 冲突和关闭意图不会被恢复逻辑撤销。Node-API 契约覆盖参数、错误、Job、取消、Buffer 和关闭顺序；Facade 与 Node 的归一化行为由同一脚本比较。`reader-replay` 采用 JSON Schema 描述的 JSONL v1，并通过虚拟 Clock、固定 Seed 和受控 Executor 在真实 Node 异步边界稳定重放取消竞态。`pixi run p3` 是统一机器验收入口。

交付内容：

- Electron Main 使用应用单实例锁；第二次启动只恢复并聚焦已有窗口，不能创建第二套 Utility Process 或打开 Workspace。
- Workspace 独占写入、崩溃后的锁恢复和明确的 Workspace Busy 行为。
- 对象文件与 SQLite 的提交协议、孤立对象清理、迁移前备份和恢复测试。
- Node-API 参数、错误、Job、取消、Buffer 和关闭顺序的契约测试。
- 在真实 Facade/Node 异步边界记录 Replay；注入 Clock、Seed 和受控 Executor。
- `reader-workspace inspect/verify/migrate --dry-run` 的首个可执行版本。

退出条件：

- 首发桌面宿主不能同时运行两个应用实例；第二次启动不会形成第二个 Workspace 写入者。
- 导入、自动保存、迁移和关闭的故障注入场景不会产生无法解释的权威数据损坏。
- Utility Process 异常退出后可由 Main 重启，陈旧结果不会覆盖新 revision。
- 同一行为套件在 Application Facade 和 Node-API 上语义一致。
- Replay 能稳定复现至少一个取消或关闭竞态，而不是只有空 Schema 和示例文件。

## 8. P4：产品 v0.1 发布

交付内容：

- 数学公式预览、图片附件、搜索、导出备份和恢复验证。
- 将最小 mutex `DocumentSession` 演进为 Runtime 管理的有界命令队列/Actor，明确 MuPDF 对象线程归属、关闭顺序和 Job 响应调度；不改变 Utility Process 隔离边界。
- 页面 display list、可见 Tile 渲染，以及页面信息、structured text/index、display list 和 Tile 的分层有界缓存。
- 当前页、相邻页、缩略图和索引的任务优先级；视口/缩放 generation 丢弃与 Tile 边界协作式取消。
- 字符或等价最小选择单元的 Unicode、范围、方向和 page-space Quad；选择结果不再由 DOM 相交行生成权威 Anchor。
- 渲染单边尺寸、总像素、Tile、缓存和跨边界 Buffer 的资源预算与稳定超限错误。
- 固定首发设备/环境上的首屏、滚动、缩放、内存和关闭预算。
- Electron 安装包、安全配置、依赖许可证清单、符号和诊断产物。
- Build once per target：测试通过的同一构建产物进入发布，不在发布阶段重新编译。

退出条件：

- 产品范围内的端到端、Fixture、Node 契约、迁移、备份恢复和 benchmark smoke 全部通过。
- 超大页、极端缩放和整数溢出输入在分配前被拒绝；测试观测到的峰值内存不超过已记录预算。
- 快速滚动和连续缩放时，旧 generation 不提交帧且能在 Tile 边界停止；关闭带待处理任务的 Runtime 不死锁、不泄漏 Buffer。
- 部分行、跨行、CJK、多栏、连字、RTL、竖排、旋转和 CropBox 选择具有结构化 Quad/Anchor 回归结果。
- 阅读热路径不要求整页 PNG 编码或在 Kernel、Node-API、IPC 间进行无预算的重复像素复制。
- 安装包可在干净的首发环境安装、启动、升级或明确拒绝不兼容版本。
- 发布产物具有 Build ID，并能找到匹配的符号、依赖版本和测试清单。
- 已知限制已经进入发布说明，没有依赖未实现宿主或未来服务的关键流程。

## 9. P5：后续宿主与扩展

按真实消费者触发，不作为产品 v0.1 的阻塞项：

- 原生/跨语言消费者：实现 `reader_c`、纯 C 契约客户端和 `reader-abi-check`。
- 自动化需求：实现产品级 CLI；Workspace Busy 时不绕过锁。
- 多客户端或远程访问：设计 `readerd`、认证、TLS、权限和速率限制。
- 扩展生态：实现 WASM Plugin Host 和能力授权。
- 内容扩展：评估 OCR、扫描件高亮和 ContextGraph。

## 10. 工具引入时机

| 工具或能力 | 首次引入 | 原因 |
| --- | --- | --- |
| 最小 Fixture manifest | P1 | 为 PDF 风险验证提供可追踪输入 |
| `reader-probe` | P1 | 观察渲染、文本和坐标事实 |
| 工作区只读验证 | P2 | 与首批持久化数据同时出现 |
| `reader-workspace` | P3 | migration、恢复和修复需求已经存在 |
| `reader-replay` | P3 | 已经存在真实 Job、事件和取消时序 |
| `reader-bench` | P4 | 已经存在可测量的完整用户路径 |
| `reader_c` / `reader-abi-check` | P5 或真实 FFI 消费者出现时 | 避免在没有消费者时冻结 ABI |

任何工具只有接入至少一条真实开发、CI、发布或诊断流程后才算完成。

## 11. 计划维护规则

- 每个阶段开始前确认输入决策，结束时保存机器可读验证证据。
- 工作量、负责人和日期在团队容量明确后补充；缺少日期不影响依赖和退出条件的约束力。
- 新需求先判断是否属于产品 v0.1；不属于则进入 P5 候选，不直接插入当前关键路径。
- 公共 API、并发模型、数据权威来源或不可逆技术选择发生变化时新增 ADR。
