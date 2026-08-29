# Context Reader 基础设施与验证策略 V0.1

状态：Draft

适用范围：开发、测试、CI、发布和故障诊断

核心目标：让复杂原生行为可观察、可重放、可比较、可验证

## 1. 设计结论

Context Reader 在产品平面之外建设独立的工程控制面：

```text
产品平面
Electron / Native / CLI / Plugin / Web
                    │
                    ▼
     Node-API / C ABI / Other Bindings
                    │
                    ▼
          Application Facade
                    │
                    ▼
                 Kernel
                    ▲
                    │
工程控制面
Fixture / Probe / Replay / Bench / ABI Check / Workspace Tool
```

产品平面负责用户能力，工程控制面负责产生工程证据。二者共享同一套公开业务契约，但不共享 UI，也不允许各自实现一套业务规则。

首阶段不建设大而全的通用基础设施框架。P1 优先开发最小 `reader-probe` 和 Fixture manifest，因为 PDF 坐标、文本结构、渲染和高亮 Anchor 是最早需要消除的风险。`reader-replay` 到 P3 再建设，此时已经存在真实的 Facade/Node-API Job、事件、取消和关闭时序。

## 2. 目标

- 将 MuPDF 和 Kernel 的关键中间结果转换为可检查的结构化产物。
- 让一个缺陷能够由文档哈希、命令、版本和随机种子稳定重现。
- 使用同一组行为验证 Application Facade 和已经实现的 Binding；产品 v0.1 首先覆盖 Node-API，C ABI 和 CLI 随真实消费者补充。
- 对正确性、性能、兼容性和数据迁移建立持续回归门槛。
- 让 AI 和人工开发都以机器可验证的事实结束，而不是以“代码看起来正确”结束。
- 从真实重复需求中提炼可复用组件，为未来原生客户端、插件 SDK 和 CLI 提供基础。

## 3. 非目标

- 不预先复制一个覆盖协程、容器、序列化、测试和 CLI 的综合工具箱。
- 不把测试工具作为绕过 Application Facade 的后门。
- 不以截图测试替代坐标、文本、Anchor 和业务契约验证。
- 不默认收集或上传用户 PDF、笔记正文、选中文字和绝对路径。
- 不在缺少固定语料、环境描述和统计方法时设置虚假的性能门槛。
- 不因为工具数量增加就立即拆分多个仓库和独立发布流程。

## 4. 从真实工作流中生长

基础设施立项需要回答三个问题：

1. 当前哪个高频或高风险问题无法稳定观察、复现或验证？
2. 工具输出将进入哪条本地开发、CI、发布或故障诊断流程？
3. 成功标准能否由机器读取，而不是依赖人工查看日志？

普通通用抽象采用“三次规则”：第一次实现留在使用方，第二次识别共同边界，出现第三个独立消费者后再考虑抽取。

Probe 和 Fixture 可以在 P1 建设，因为它们直接降低最早的 PDF 产品风险；Replay 在 P3 已经出现真实异步时序后建设。其内部公共库仍遵守真实重复后再抽取的规则。

本策略借鉴以下项目的工程思想：

- [catter](https://github.com/clice-io/catter)：从获取可靠编译事实的实际问题出发，逐步扩展为可观察和修改构建过程的工具。
- [kotatsu](https://github.com/clice-io/kotatsu)：从 clice 的重复需求提炼协程、IPC、序列化、CLI 和测试能力，并通过 Recording Transport 支持记录与重放。
- [clice](https://github.com/clice-io/clice)：让 CI 验证的构建产物直接进入发布流程，并保留精确匹配的诊断信息。
- [cxx-toolchains](https://github.com/clice-io/cxx-toolchains)：将可验证事实表达为机器可读数据，分离事实、推导和消费代码。

借鉴的是问题驱动、机器可读和可重现的原则，不绑定这些项目的具体实现或技术选型。

## 5. 边界与依赖规则

### 5.1 默认路径

工程工具默认调用正式边界：

```text
tool -> Application Facade -> Kernel
Node contract runner -> Node-API -> Application Facade -> Kernel
future FFI tool -> C ABI -> Application Facade -> Kernel
```

这样验证的是客户端真实使用的产品契约，而不是某个内部类的偶然行为。

### 5.2 PDF 诊断例外

页面布局、字符 Quad 和坐标变换需要比业务 API 更细的诊断信息。PdfModule 可以提供开发构建专用、版本化的 `PdfDiagnosticApi`：

- 返回项目拥有的 DTO，不暴露 `fz_*` 等 MuPDF 类型。
- 只读，不修改文档、Workspace 或缓存权威状态。
- 与生产 Application Facade 分开组装，可从发布构建中关闭。
- schema 变化必须显式升级，不允许静默改变坐标含义。

### 5.3 工作区维护例外

`reader-workspace` 通过版本化 Maintenance API 执行离线检查和修复：

- 写操作前验证 Workspace 锁和 schema 版本。
- 默认只读；修复必须显式指定 `--apply`。
- 支持 `--dry-run` 并输出计划修改。
- 修改前创建可恢复备份，并在完成后重新验证不变量。
- 不将内部 SQLite schema 承诺为第三方公共 API。

### 5.4 禁止依赖

```text
tools -X-> Kernel 私有类
tools -X-> 其他模块的 SQLite 表
tools -X-> MuPDF 裸句柄
tools -X-> Electron Renderer
kernel -X-> tools
```

工具之间通过文件契约或小型公共库组合，不形成按启动顺序耦合的常驻服务网络。

## 6. 通用命令约定

所有命令行工具遵守共同约定：

```text
--help                 显示命令和退出码
--version              显示工具、Kernel 和契约版本
--format json|jsonl    选择机器可读输出
--output <path>        指定产物目录或文件
--seed <uint64>        固定随机行为
--timeout <duration>   设置确定的超时
--log-level <level>    控制诊断日志
--redact               强制隐私脱敏
--dry-run              仅对具有写能力的工具提供
```

标准输出只承载请求的结果；诊断日志写入标准错误。成功为 `0`，使用错误、验证失败、超时、环境失败和内部错误使用稳定且有文档的不同退出码。

任何失败输出都应包含一条可直接执行的最小重现命令，或者说明为何无法生成。

## 7. 通用产物契约

每次运行生成 `run-manifest.json`：

```json
{
  "schema_version": 1,
  "tool": {"name": "reader-probe", "version": "0.1.0"},
  "kernel": {"version": "0.1.0", "build_id": "...", "abi_version": 1},
  "dependencies": {"mupdf": "..."},
  "platform": {"os": "...", "arch": "...", "compiler": "..."},
  "input": {"document_sha256": "..."},
  "invocation": {"arguments": [], "seed": 1},
  "result": {"status": "passed", "duration_ms": 0}
}
```

约束：

- schema 使用独立版本，不与 Kernel semantic version 混为一体。
- 时间统一使用 UTC；持续时间使用单调时钟测量。
- 浮点坐标定义单位、原点、方向和比较容差。
- 大型二进制产物通过相对路径和 SHA-256 引用，不内嵌进 JSON。
- JSON 用于有层次的有限结果，JSONL 用于事件流、Trace 和大批量记录。
- 产物目录必须可以整体归档，不能依赖生成机器上的绝对路径。

## 8. reader-probe

### 8.1 职责

`reader-probe` 将 PDF 管线的中间状态导出为可检查产物：

```text
probe-output/
  run-manifest.json
  document.json
  page-0001.png
  page-0001.layout.json
  page-0001.text.txt
  coordinates.jsonl
  selections.jsonl
  anchors.jsonl
  trace.jsonl
```

首批命令：

```text
reader-probe inspect <pdf>
reader-probe render <pdf> --page 1 --scale 2
reader-probe text <pdf> --page 1
reader-probe select <pdf> --page 1 --from ... --to ...
reader-probe anchor <pdf> --selection <selection.json>
reader-probe roundtrip <pdf> --page 1
reader-probe compare <actual-dir> <baseline-dir>
```

### 8.2 必须验证的不变量

- 页面、PDF、设备像素和屏幕坐标类型不能隐式混用。
- 坐标矩阵正向和逆向转换在定义容差内往返一致。
- Selection 的 Quad 有限、方向合法并位于合理页面边界内。
- 文本顺序、字符范围和 Quote Anchor 之间可以相互追踪。
- 相同版本和选项产生相同的结构化结果；允许差异必须有显式规则。
- 高亮覆盖层与渲染位图使用相同页面变换定义。

PNG 主要用于人工诊断和有限的视觉比较。正确性门槛优先建立在结构化布局、几何和 Anchor 不变量上。

## 9. reader-fixture

### 9.1 语料结构

```text
tests/corpus/
  manifest.json
  public/
  generated/
  private-local/
  expectations/
```

每个 Fixture 至少描述：

```text
fixture_id
document_sha256
origin
license
redistributable
features[]
expected_outcome
related_issue
added_by_change
```

`features` 可以包含 CJK、RTL、竖排、双栏、连字、旋转页面、CropBox、透明度、表单、扫描件、加密、损坏和超大页面。

### 9.2 管理规则

- 回归缺陷先固化最小输入，再修复实现。
- 能程序生成的边界 PDF 优先记录生成器和种子。
- 私有用户文件不能进入仓库或远程 CI；只保存脱敏最小样本或本地哈希引用。
- 更新预期结果必须生成语义差异报告，不能使用无审查的批量接受。
- Fixture 的授权和可再分发状态必须可机器检查。

首批命令：

```text
reader-fixture verify
reader-fixture list --feature cjk
reader-fixture add <pdf> --license ...
reader-fixture generate --case rotated-cropbox --seed 1
reader-fixture bless <probe-output> --reviewed-by ...
```

## 10. reader-replay

### 10.1 记录边界

Replay 在 Application Facade、已经实现的 Binding 或 Host IPC 边界记录：

```json
{"type":"request","sequence":1,"method":"document.open","request_id":"..."}
{"type":"event","sequence":2,"name":"page.ready","job_id":"..."}
{"type":"response","sequence":3,"request_id":"...","status":"ok"}
```

记录内容包括方法、稳定 ID、参数摘要、结果状态、事件顺序、任务生命周期、取消和相对时间。不默认记录 PDF 字节、选中文字和笔记正文。

### 10.2 确定性模型

Replay 必须允许注入 Fake Clock、固定随机种子、受控 Executor 和内容哈希映射。比较分为：

- 严格字段：错误码、revision、事件因果顺序、资源生命周期。
- 规范化字段：临时 ID、时间戳、平台路径。
- 容差字段：耗时、浮点坐标和平台相关渲染差异。
- 忽略字段：明确标记为非契约的诊断信息。

首批命令：

```text
reader-replay record --boundary facade|node --scenario <scenario>
reader-replay run <session.jsonl>
reader-replay compare <expected.jsonl> <actual.jsonl>
reader-replay minimize <failing-session.jsonl>
```

## 11. reader-bench

基准测试使用固定语料和场景定义：

```text
ColdOpen
FirstPageVisible
RenderVisibleTile
ExtractPageText
CreateAnnotation
SearchLargeWorkspace
FastScrollCancellation
ShutdownWithPendingJobs
```

结果必须包含样本数量、预热次数、中位数、P95、内存峰值和环境元数据。基准比较区分：

- PR smoke：快速发现数量级退化，不做精细统计结论。
- Nightly benchmark：固定设备、重复运行、保存趋势。
- Release qualification：使用候选发布产物和版本化性能预算。

性能门槛必须针对场景设置，不使用一个全局百分比。变化超过阈值时保存 Probe、Trace 和构建产物引用，便于继续诊断。

## 12. reader-abi-check

`reader-abi-check` 在出现真实 FFI 消费者并开始实现 C ABI 后引入，使用纯 C 测试客户端和多个宿主组合验证：

- ABI 和 struct size 版本协商。
- opaque handle 的创建、释放、重复释放防护和错误句柄处理。
- Buffer 成功、失败、取消后的唯一释放路径。
- exception 不穿越 ABI。
- UTF-8、固定宽度数值、空值和超大输入。
- Job 取消、超时、回调线程和 Runtime 关闭顺序。
- 旧客户端连接新 Kernel，以及受支持的新客户端连接旧 Kernel。
- Node-API 和 CLI 对相同契约测试保持语义一致。

ABI Header 可额外接入结构布局快照和二进制符号检查，但行为契约仍由真实调用测试证明。

## 13. reader-workspace

首批命令：

```text
reader-workspace inspect <workspace>
reader-workspace verify <workspace>
reader-workspace migrate <workspace> --dry-run
reader-workspace migrate <workspace> --apply
reader-workspace rebuild-index <workspace>
reader-workspace repair <workspace> --plan <plan.json>
reader-workspace repair <workspace> --plan <plan.json> --apply
```

验证范围包括 schema、外键、revision、对象哈希、缺失对象、孤立附件、索引版本和缓存键。`repair` 必须使用可审查计划文件，禁止静默猜测如何修复权威数据。

## 14. 后续工具

以下能力按对应产品阶段和真实需求引入：

- Fault Injection（P3）：模拟分配失败、I/O 中断、损坏对象、进程退出和取消竞态。
- `reader-pack`（P4）：可重复打包、构建清单、符号文件、许可证和 SBOM。
- Fuzz Harness：针对 PDF 导入边界、C ABI 解码、Markdown/公式和交换格式。
- Compatibility Matrix：跨 Kernel、Workspace schema、插件 API 和 Host 版本执行兼容测试。

这些能力可以是现有测试目标或 CI Job，不要求每项都成为独立可执行文件。

## 15. CI 与发布

建议流水线：

```text
Build once
   │
   ├── Unit + Static Analysis
   ├── Fixture + Probe Regression
   ├── Facade + Node Contract
   ├── Replay                    (P3 起)
   ├── Workspace Migration       (P3 起)
   ├── Benchmark Smoke           (P4 起)
   └── C ABI Contract            (真实 FFI 消费者出现后)
           │
           ▼
Archive exact binaries + symbols + manifests
           │
           ▼
Promote the tested artifacts to release
```

原则：

- 每个目标平台和架构分别构建一次；测试和发布使用对应的同一个二进制产物，不在发布阶段重新编译。
- 每个归档制品具有 Build ID，可找到精确匹配的符号和依赖版本。
- 失败时保留最小必要产物；涉及私有 Fixture 时只保留哈希和脱敏诊断。
- Baseline 变更独立审查，并附带差异原因和生成命令。
- 不稳定测试必须被登记、定责和限期处理，不能通过无限重试制造绿色结果。

## 16. 仓库布局

初期保持 Monorepo：

```text
context-reader/
  kernel/
  bindings/
  hosts/
  contracts/
    diagnostic/
    replay/
    run-manifest/
  tools/
    probe/
    fixture/
    replay/
    bench/
    abi-check/
    workspace/
  tests/
    corpus/
    contract/
    integration/
  baselines/
```

保持同仓库可以让契约变更、实现和验证在一个提交中原子演进。满足以下至少一项时才考虑拆分仓库：

- 工具拥有 Kernel 之外的稳定消费者。
- 工具需要独立发布、兼容性或安全响应周期。
- 数据集体积、授权或访问控制要求独立管理。
- 工具已经具有清晰、稳定且不依赖内部源代码的公共契约。

拆分后仍通过版本化契约和发布制品集成，不能依赖源码目录相对路径。

## 17. 组合优先的实现方式

工程工具同样采用组合：

```text
CLI Command
  -> Scenario Runner
  -> Facade/Binding Client
  -> Artifact Writers
  -> Comparators
```

建议的小型组件包括 `RunManifestWriter`、`JsonlEventSink`、`ArtifactStore`、`Redactor`、`ResultComparator` 和 `FixtureCatalog`。只有在多个工具中产生真实重复后才抽取到共享 target。

避免建立 `ToolBase`、`TestManager`、`UniversalRunner` 或依赖全局注册的插件式测试框架。不同工具共享数据契约和小型组件，不通过继承树共享控制流。

## 18. 典型工作流

### 18.1 PDF 坐标缺陷

```text
用户问题
  -> 脱敏并加入 Fixture
  -> reader-probe 导出布局和坐标往返
  -> 写结构化不变量测试
  -> 修复 PdfModule
  -> Facade/已实现 Binding 契约回归
  -> 保存最小诊断产物
```

### 18.2 偶发异步缺陷

```text
记录 Facade/IPC Session
  -> 固定 Clock、Seed、Executor
  -> Replay 复现
  -> Minimize 删除无关请求
  -> 增加任务生命周期测试
  -> 在所有 Binding 重放
```

### 18.3 性能退化

```text
Nightly 趋势越界
  -> 确认同一 Build 和环境
  -> 重复场景排除噪声
  -> 查看 Trace / Probe 产物
  -> 优化并比较候选 Build
  -> 审查后更新或保持预算
```

### 18.4 工作区迁移失败

```text
旧版本 Fixture Workspace
  -> migrate --dry-run
  -> 备份
  -> 执行迁移
  -> verify 权威数据不变量
  -> 重建派生索引
  -> 使用旧版数据快照做回归
```

## 19. 与产品实施阶段对齐

基础设施没有独立于产品的里程碑，其引入时机由 [实施计划 V0.1](implementation-plan-v0.1.md) 决定：

### P1：PDF 风险验证

- 建立最小 Fixture manifest、授权规则和 `reader-probe inspect/render/text/roundtrip`。
- 为页面坐标、文本布局和 Anchor 建立结构化不变量测试。
- 只定义 Probe 当前真正需要的 Run Manifest 字段，不先做通用工具框架。

### P2：Electron 纵向闭环

- 在持久化首次出现时同步建立 Workspace 只读不变量检查。
- 同一行为场景覆盖 Application Facade 与 Node-API。
- CI 归档失败所需的最小 Probe、Fixture 和工作区验证产物。

### P3：数据与边界稳定

- 实现 Replay JSONL schema，并记录已经存在的 Facade/Node 请求、事件、Job、取消和关闭时序。
- 使用受控 Clock、Seed 和 Executor 重放至少一个真实异步缺陷场景。
- 实现 Workspace inspect/verify/migrate 和故障恢复验证。

### P4：产品 v0.1 发布

- 建立场景、固定语料、环境描述和产品性能预算。
- PR 执行 benchmark smoke，固定设备执行 nightly 趋势。
- 发布流程提升已经完整验证的同一目标构建产物并归档符号和 manifests。

### P5 或真实 FFI 消费者出现时

- 实现 C ABI 生命周期、兼容性矩阵和 `reader-abi-check`。
- 将同一行为契约套件扩展到 C ABI 和相应原生宿主。

## 20. 完成标准

基础设施能力只有满足以下条件才算完成：

- 解决一个明确的开发、CI、发布或诊断问题。
- 使用版本化、机器可读的输入输出契约。
- 失败能够生成最小重现命令或明确说明限制。
- 包含自身的单元、集成或契约测试。
- 不复制 Kernel 业务规则，不泄漏私有类型和数据库结构。
- 隐私、授权、产物保留和清理规则已经定义。
- 已接入至少一条真实工作流，而不是只存在于设计文档中。

## 21. 决策门槛

- P1 基线入库前：公共 PDF Fixture 的来源、授权审核和二进制存储方式。
- P1 比较命令启用前：渲染差异采用结构化几何、像素阈值、感知差异或有限人工诊断的组合。
- P3 Replay 实现前：Run Manifest 和 Replay schema 采用 JSON Schema 还是代码生成 IDL。
- P4 性能门槛启用前：Nightly Benchmark 的固定设备和历史结果存储位置。
- P4 发布前：Windows x64 的符号归档、崩溃文件和 Build ID 方案；其他 OS 在进入支持矩阵前分别决定。
- P5 C ABI 实现前：开发构建专用 `PdfDiagnosticApi` 是否确有 FFI 诊断消费者需要通过 C ABI 访问。
