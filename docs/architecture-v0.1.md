# Context Reader 架构设计 V0.1

状态：Draft

适用范围：首个可用版本及其后续演进

目标技术：Windows x64、C++20、Pixi、CMake 3.28+、MSYS2 UCRT64、MuPDF、SQLite、Electron（首发宿主）

## 1. 设计结论

Context Reader 采用“一个 Kernel、一套 Application Facade、多种 Binding/Host”的模块化单体架构。

首发产品是 Electron 桌面客户端。Electron 的 Node-API Binding 与 Kernel 使用同一工具链构建，直接调用 C++ Application Facade；C ABI 是后续原生客户端和跨语言嵌入使用的并列 Binding，不位于 Electron 的内部调用路径上。

```text
Electron / Native / CLI / Plugin / Web
                  │
        Binding / Transport Adapters
     Node-API / C ABI / CLI / WASM / HTTP
                  │
          Application Facade
                  │
             Reader Kernel
                  │
       Ports + Infrastructure Adapters
        MuPDF / SQLite / File System
```

Kernel 是唯一业务规则来源。不同客户端不能拥有自己的文档、批注、笔记或搜索实现。

“唯一”不表示全局单例。一个进程可以创建多个 `ReaderRuntime`，但同一个 Workspace 同时只允许一个写入者。

## 2. 目标

- 支持 PDF 导入、阅读、文本选择、高亮和语境笔记。
- 笔记保存 Markdown 原文，支持数学公式和图片附件。
- 首先通过 Electron 完成可安装、可恢复的单用户桌面阅读闭环。
- Electron 不成为业务边界，未来可以替换成原生桌面或移动端 UI。
- CLI、插件和自动化复用同一套业务用例。
- MuPDF 崩溃、损坏 PDF 和第三方插件尽可能被隔离。
- 数据格式、ABI 和公共契约能够独立演进和迁移。
- 架构适合 AI 辅助开发，但所有生成代码受到模块边界、测试和 CI 约束。

## 3. 非目标

产品 v0.1 不包含以下承诺：

- 不实现微服务架构。
- 不实现多人实时协作和 CRDT。
- 不在每次高亮时写回原 PDF。
- 不为第三方插件开放数据库、任意文件系统或 MuPDF 对象。
- 不保证一个 Web UI 插件可以自动变成原生 UI 插件。
- 不在缺少性能数据时引入 GPU 纹理共享或复杂共享内存协议。
- 不承诺同时交付原生客户端、Web、Daemon、插件和产品级 CLI。
- 不支持 OCR、扫描 PDF 文本识别和 ContextGraph；扫描 PDF 在首版只作为不可选中文本的页面渲染。

## 4. 架构原则

### 4.1 Kernel First

领域模型、业务校验、事务、权限、并发控制和持久化协调属于 Kernel。UI 只拥有布局、交互和临时显示状态。

### 4.2 Application Facade 是唯一用例入口

所有宿主最终调用同一个 Facade：

```text
ImportDocument
OpenDocument
RenderPage
GetTextLayout
CreateAnnotation
UpdateNote
Search
Export
```

Binding 只负责参数转换、线程切换、内存桥接和错误映射，不实现业务规则。

### 4.3 Composition First

Kernel 由模块组合，模块由用例组合，用例由 Port 组合，生产环境由 Composition Root 组装。

不建立 `BaseService`、`BaseModule` 或多层业务继承树。

### 4.4 Stable IDs

标题、文件名和物理路径不是身份。系统使用稳定、不透明的 ID：

```text
WorkspaceId
DocumentId
DocumentVersionId
AnnotationId
NoteId
AssetId
JobId
```

### 4.5 权威数据与派生数据分离

结构化业务数据和原始 PDF/附件是权威数据。页面位图、缩略图、全文索引和文本布局缓存是可重建数据。

### 4.6 本地直接调用，服务模式可选

Electron 通过 Node-API 直接调用 C++ Application Facade；本地原生应用在后续需要跨语言嵌入时使用 C ABI/FFI。HTTP 仅用于 Web、自托管、远程访问和多客户端共享。

### 4.7 工程控制面是一等架构

Fixture、Probe、Replay、Bench 和 ABI Check 不是临时脚本，而是围绕 Kernel 公共契约建设的工程控制面：

```text
产品平面                              工程控制面
Electron / Native / CLI / Plugin      Fixture / Probe / Replay
                 │                    Bench / ABI Check / Workspace Tool
                 └──────────┬─────────┘
                            ▼
             Implemented Binding / Application Facade
                            ▼
                         Kernel
```

工程工具不能成为第二套业务实现。除专用离线维护接口和 PdfModule 的受控诊断接口外，工具不得直接访问数据库表、MuPDF 句柄或 Kernel 私有类。

基础设施从真实工作流中生长：PDF 风险验证阶段优先建设最小 Probe 和 Fixture；Replay 在异步 Job、事件和 Binding 边界实际出现后建设。只有出现多个稳定消费者后，才将重复实现提炼为通用库。

## 5. 运行形态

### 5.1 Electron 桌面模式

```text
Electron Renderer
       │ typed IPC
       ▼
Electron Utility Process
       │ Node-API
       ▼
C++ Application Facade
       │
       ▼
   reader_core
```

Renderer 保持沙箱化，不能加载原生模块。Utility Process 加载 `reader_node`；`reader_node` 与 `reader_core` 使用同一 C++20 工具链和构建产物，直接调用 Application Facade。它只负责参数转换、异步桥接、线程切换、Buffer 生命周期和错误映射，不实现业务规则。Utility Process 负责执行耗时和不可信 PDF 工作，进程退出后由 Electron Main 监管并重启。

### 5.2 原生嵌入模式

```text
Swift / Kotlin / C# / Rust / Qt
              │ C ABI / FFI
              ▼
          reader_c
              ▼
 C++ Application Facade
              ▼
          reader_core
```

移动端和原生桌面端可以将 Kernel 编译为静态库或动态库。宿主承担 Kernel 崩溃会终止自身进程的风险。

### 5.3 CLI 模式

`reader_cli` 直接链接 Application Facade，不重新实现 CRUD。若目标 Workspace 已被其他进程打开，CLI 应连接其 daemon，或明确返回 Workspace Busy，不能绕过锁直接打开数据库。

CLI 不进入 Electron 首发关键路径。首个 CLI 版本可以只返回 Workspace Busy；连接 daemon 的能力随 `readerd` 一并设计，不能为了首发预建一套常驻服务。

### 5.4 Daemon/Web 模式

`readerd` 是可选 Host。它可以暴露 HTTP/OpenAPI、SSE 和 WebSocket，但不得成为 Kernel 的依赖。

远程模式必须独立设计 TLS、认证、权限和速率限制；不能直接暴露桌面本地端点。

## 6. 逻辑分层

```text
domain
  领域实体、值对象、领域规则、领域事件

application
  用例、事务边界、权限检查、任务编排、Application Facade

ports
  PdfEngine、Repository、AssetStore、Clock、Executor 等接口

adapters
  MuPDF、SQLite、文件系统、日志、平台密钥库

bindings / hosts
  C ABI、Node-API、CLI、插件、HTTP
```

依赖方向只能指向更内层：

```text
hosts -> application -> domain
adapters -> ports <- application
```

领域层不能包含 MuPDF、SQLite、Electron、Node、HTTP 或平台路径类型。

## 7. Kernel 模块

### 7.1 SystemModule

负责运行时版本、能力发现、健康状态、诊断信息和兼容性协商。

### 7.2 WorkspaceModule

负责 Workspace 创建、打开、独占锁、数据库迁移、备份、恢复和导入导出。

### 7.3 DocumentModule

负责 PDF 导入、内容哈希、不可变文档版本、元数据、收藏和文档生命周期。

### 7.4 PdfModule

负责页面信息、文本布局、坐标映射、区域命中、页面/Tile 渲染和密码文档处理。MuPDF 只能存在于该模块的 adapter 中。

### 7.5 AnnotationModule

负责高亮、Anchor、样式、批注状态、删除和文档版本变化后的重新锚定。

### 7.6 NoteModule

负责 Markdown 原文、数学公式方言、附件引用、revision、自动保存和笔记与语境的关联。

### 7.7 AssetModule

负责图片和附件的内容寻址存储、MIME 校验、大小限制、去重和垃圾回收。

### 7.8 SearchModule

负责 PDF 文本和笔记全文搜索。搜索索引是派生数据，可以重建。

### 7.9 JobModule

负责导入、索引、OCR、导出等长任务的状态、优先级、进度、取消和错误恢复。

### 7.10 PluginModule

负责插件清单、能力授权、生命周期、隔离存储、事件和 WASM Host API。该模块不进入首个阅读闭环的关键路径。

### 7.11 ContextGraphModule（可选）

思维导图、知识图谱和摘录关系属于可选模块，不应与 Annotation 或 Note 的基本生命周期绑定。

## 8. 核心领域模型

### 8.1 Document 与 DocumentVersion

`Document` 表示用户资料库中的逻辑文档；`DocumentVersion` 表示不可变 PDF 内容。

```text
Document
  id
  title
  active_version_id

DocumentVersion
  id
  document_id
  content_hash
  object_key
  byte_length
  page_count
  created_at
```

相同 PDF 可以通过内容哈希去重。用户替换文件时创建新版本，不覆盖旧版本。

### 8.2 Anchor

高亮不能只保存屏幕矩形。Anchor 至少包含：

```text
Anchor
  document_version_id
  page_index
  quads[]
  text_range
  quote.exact
  quote.prefix
  quote.suffix
  layout_version
```

`quads` 使用规范化 PDF 页面坐标，不保存屏幕像素。`exact/prefix/suffix` 用于文档更新后的重新定位。

### 8.3 Annotation 与 Note

Annotation 表示视觉标记和语境目标；Note 表示可独立演进的 Markdown 内容。二者通过稳定 ID 关联，不把完整笔记塞进高亮记录。

Note 保存：

```text
markdown_source
revision
created_at
updated_at
deleted_at
```

产品 v0.1 的 Markdown 方言固定为 CommonMark + GFM 子集 + 行内/块级数学公式，编辑方式采用源码编辑加预览。原始 HTML 默认禁用。

## 9. 数据与工作区

建议工作区结构：

```text
workspace/
  workspace.lock
  workspace.db
  index.db
  objects/
    pdf/<prefix>/<sha256>.pdf
    asset/<prefix>/<sha256>
  cache/
    render/
    thumbnail/
  plugins/
    <plugin-id>/
  backups/
```

- `workspace.db` 是结构化权威数据，只能由 Kernel 写入。
- `objects/` 保存不可变 PDF 和附件。
- `index.db`、`cache/` 可以删除并重建。
- 数据库迁移由 WorkspaceModule 独占执行，迁移前创建可恢复备份。
- 对外定义版本化 `readerpkg` 交换格式，第三方工具不依赖内部 SQLite 表。

## 10. PDF 与渲染架构

### 10.1 DocumentSession Actor

每个活动 PDF 由一个 `DocumentSession` 串行管理文档访问、文本布局和 display list 创建。渲染任务通过 Runtime 管理的 Executor 执行。

```text
DocumentSession
  document handle
  page metadata cache
  text layout cache
  display list cache
  cancellation state
```

MuPDF 上下文、文档和设备的线程规则封装在 MuPdfEngine 内，不能要求上层调用者正确加锁。

### 10.2 页面分层

客户端页面建议采用：

```text
交互层
批注覆盖层
透明文本选择层
PDF 位图层
```

高亮不合成进 PDF 位图，修改批注时不触发页面重渲染。

### 10.3 调度与缓存

任务优先级：

```text
当前可见 Tile > 相邻页面 > 缩略图 > 全文索引
```

渲染缓存键至少包含：

```text
document_version + page + scale_bucket + tile + DPR + render_options
```

所有长任务必须支持取消。超出视口或缩放版本已过期的渲染结果应被丢弃。

## 11. Application Facade

Facade 按用例组织，不提供“获取内部模块”或“执行任意 SQL”。示意：

```cpp
class ReaderApplication final {
public:
    JobHandle<DocumentSummary> import_document(const ImportDocument& command);
    JobHandle<DocumentSessionInfo> open_document(const OpenDocument& command);
    Result<PageInfo> page_info(const GetPageInfo& query) const;
    JobHandle<RenderedPage> render_page(const RenderPage& command);
    Result<Annotation> create_annotation(const CreateAnnotation& command);
    Result<Note> update_note(const UpdateNote& command);
};
```

Facade 负责进入用例边界；具体工作委托给组合后的用例对象。涉及文件 I/O、PDF 打开/渲染、索引、OCR 和导出的操作统一返回可取消 Job；已经具备前置数据、能够在短事务内完成的状态修改返回 `Result<T>`。Binding 不得把长任务重新包装成阻塞调用。

## 12. 公共调用边界

### 12.1 C ABI

C ABI 是后续原生客户端和跨语言嵌入的稳定边界，不是 Electron 首发的内部调用层。它作为 Application Facade 的独立 Binding，在出现第一个真实 FFI 消费者时实现和稳定化：

- 仅导出 `extern "C"`。
- 使用 opaque handle，不暴露 C++ 类。
- 字符串统一 UTF-8。
- 数字使用固定宽度类型。
- Buffer 明确所有权和释放函数。
- 异常禁止穿越 ABI。
- 结构体包含 `struct_size` 和 API version。
- 长任务返回 Job Handle，支持进度和取消。

### 12.2 控制面与数据面

控制面处理文档、批注、笔记和搜索命令，少量结构化序列化可以接受。

数据面处理页面像素、Tile、图片和文本布局，提供专用 Buffer API，减少复制。共享内存只有在基准测试确认必要时引入。

### 12.3 Node-API

`reader_node` 是 C++ Application Facade 的薄包装，与 `reader_c` 并列：

- 不包含业务校验。
- 不直接执行同步耗时工作。
- 将 Job 转为 Promise/Event。
- 将 Kernel Buffer 包装为外部 ArrayBuffer，并绑定正确的释放回调。
- 与 `reader_core` 一起构建和发布，不承诺二者之间的二进制 ABI 稳定性。
- Node-API 与未来 C ABI 运行同一组行为契约，但分别验证各自的参数、错误和生命周期映射。

## 13. 事件、事务与并发

### 13.1 事件分层

- Domain Event：模块内部语义，不直接暴露给客户端。
- Application Event：跨模块协调。
- Client Event：稳定、版本化的外部通知。

外部事件包含 `event_id`、`workspace_id`、`revision`、`type` 和 payload version。

### 13.2 事务

一个用例拥有一个明确事务边界。状态修改和待发布事件写入同一事务；事务提交后再向客户端分发事件。

模块不得直接更新其他模块的表。跨模块一致性由 Application Coordinator 调用公开 Port 或用例完成。

### 13.3 并发

- SQLite 初期采用单写者，读取连接按需扩展。
- DocumentSession 串行拥有 MuPDF document。
- Runtime 统一拥有线程池，不允许模块私自无限创建线程。
- 修改命令携带 `expected_revision`，使用乐观并发。
- 所有异步任务支持取消，关闭 Runtime 时有确定的停止顺序。

## 14. 插件架构

建议插件包：

```text
plugin.zip
  plugin.json
  ui/web.js          可选
  kernel/plugin.wasm 可选
  assets/
```

插件清单声明版本、平台和权限：

```text
document.read
page.text.read
annotation.read
annotation.write
note.read
note.write
asset.write
network
```

Kernel 插件通过 WASM Host Capability API 工作，不能链接 C++ 内部 ABI、读取数据库或持有 MuPDF 指针。

UI 插件与 Kernel 插件是两个运行时。跨宿主复用的 UI 扩展优先使用声明式命令、菜单和面板贡献；复杂 UI 允许提供宿主专用实现。

## 15. 安全边界

- PDF、Markdown、图片、插件和深链均视为不可信输入。
- Electron Renderer 禁用 Node Integration，启用 context isolation、sandbox 和 CSP。
- 文件选择由 Host 完成，Renderer 不获得任意文件系统能力。
- Markdown 禁止原始脚本和危险 URL，外部链接经过策略检查。
- 图片导入限制尺寸、字节数和 MIME，防止解码炸弹。
- 本地服务若启用，使用随机端点和会话令牌；远程服务使用独立认证方案。
- 日志默认不记录笔记原文、选中文字和本地绝对路径。

## 16. 可观测性

每个跨边界请求具有 request ID。结构化日志至少包含：

```text
timestamp
severity
module
operation
request_id
document_id（可脱敏）
duration
error_code
```

支持生成脱敏诊断包，包括版本、能力、数据库 schema、任务状态和最近错误，不包含用户正文。

## 17. 工程控制面

工程控制面负责将 PDF 处理、跨语言调用、异步执行和数据迁移变成可检查、可重现的机器可读事实。首批工具包括：

```text
reader-probe       导出页面渲染、文本布局、坐标变换和 Anchor 诊断产物
reader-fixture     管理 PDF 回归语料、来源、哈希、特征和预期结果
reader-replay      记录并重放 Facade/Binding 请求、事件、结果和时序
reader-bench       在固定语料和环境上执行性能基准与趋势比较
reader-abi-check   验证 C ABI 版本、所有权、取消、并发和错误契约
reader-workspace   检查、迁移、修复和重建工作区派生数据
```

所有工具遵守以下边界：

- 默认通过 Application Facade 或 C ABI 观察产品真实行为。
- PdfModule 可以提供开发构建专用的版本化 `PdfDiagnosticApi`，但不暴露 MuPDF 类型。
- 离线维护通过版本化 Maintenance API 工作，并要求 Workspace 未被其他进程写入。
- 输出优先采用带 schema version 的 JSON/JSONL；图像使用 PNG；工具提供稳定退出码。
- 回归产物记录 Kernel、C ABI、MuPDF、构建 ID、平台、文档哈希、命令、随机种子和耗时。
- 默认不在日志和重放文件中保存 PDF 正文、笔记正文、选中文字或本地绝对路径。

详细设计见 [基础设施与验证策略 V0.1](infra-strategy-v0.1.md)。

## 18. 测试策略

### 18.1 Kernel 单元测试

使用内存 Repository、Fake PdfEngine、Fake Clock 和 Inline Executor 测试领域规则与用例。

### 18.2 PDF 回归语料

至少覆盖中文、连字、RTL、竖排、多栏、旋转页、CropBox、扫描页、加密 PDF、超大页和损坏文件。

### 18.3 契约测试

同一组行为测试分别运行在 Application Facade 和已经实现的 Binding 上，确保 Binding 没有改变语义。产品 v0.1 必须覆盖 Node-API；C ABI 和 CLI 在对应消费者出现后加入同一套测试。

### 18.4 端到端测试

覆盖导入 PDF、选择文本、创建高亮、编辑 Markdown、插入图片、重启恢复和导出备份的完整流程。

### 18.5 性能测试

在固定设备和固定 PDF 语料上定义首屏、滚动、缩放、内存和大文档索引预算。优化必须提供前后基准数据。

## 19. 版本兼容

需要独立管理：

```text
Kernel semantic version
C ABI version
Workspace schema version
Plugin API version
readerpkg format version
diagnostic artifact schema version
replay format version
```

新增字段默认兼容；删除或改变语义需要迁移或 major version。客户端启动时通过 Capability API 协商能力，不通过版本字符串猜测功能。

## 20. 建议实施顺序

实施以能够运行和验收的纵向阶段组织，不按模块或工具数量衡量进度：

1. P0 决策与工程骨架：验证独立 MSYS2 UCRT64 GCC 工具链，确认 MuPDF 许可和 Electron/Node 版本策略，建立由 Pixi 驱动的 Windows x64 最小 CMake/CI。
2. P1 PDF 风险验证：实现 PdfEngine Port、MuPDF Adapter、最小 Probe 和首批 Fixture，验证渲染、文本布局和坐标往返。
3. P2 Electron 首个纵向闭环：实现最小 Workspace、Document、Annotation、Note、`reader_node` 和 Electron Host，完成导入、阅读、选择、高亮、笔记与重启恢复。
4. P3 数据与边界稳定：补齐锁恢复、迁移、备份不变量、Node-API 契约测试和真实异步场景的 Replay。
5. P4 产品 v0.1 发布：完成产品范围内的数学公式、图片附件、搜索、导出备份、性能预算、安装包和发布验证。
6. P5 后续宿主：出现真实 FFI 消费者后实现 C ABI 和 ABI Check；CLI、原生客户端、readerd、插件、OCR 和 ContextGraph 按需求引入。

每个阶段必须满足机器可验证的退出条件。详细范围、依赖和验收标准见 [实施计划 V0.1](implementation-plan-v0.1.md)。

## 21. 决策门槛

已经确定：

- 首发产品为 Electron 桌面客户端。
- 首发平台为 Windows x64。
- Scoop 只负责引导安装 Pixi 和独立 MSYS2；Pixi 负责锁定 CMake、Ninja 等开发工具和项目任务环境。
- 构建系统使用 CMake 3.28+；第三方依赖由 CMake `FetchContent`、`add_subdirectory` 或受控 `ExternalProject` 获取和编排，不引入 vcpkg 或 Conan。
- 首选编译器候选为独立 MSYS2 UCRT64 GCC 16.2.0；其 Electron Node-API 兼容性由 P0 原生模块加载测试决定。
- `reader_node` 直接调用 C++ Application Facade，不经过 C ABI。
- Kernel 和一方 C++ Binding 统一使用 C++20 编译；已有 C++17 风格代码作为 C++20 的兼容子集纳入。
- 首版 Markdown 使用源码编辑加预览，不引入富文本双向转换。
- OCR、扫描 PDF 矩形高亮和 ContextGraph 不进入产品 v0.1。

尚未确定的事项按最晚决策阶段处理：

- P0 结束前：使用独立 MSYS2 UCRT64 GCC 16.2.0 通过目标 Electron 的 `.node` 模块加载测试，或根据测试结果改用 MSVC/clang-cl；同时确认 Node/Electron 版本策略、MuPDF 开源或商业许可方案，并验证 MuPDF 在 Windows 上的 CMake 编排方式。
- P1 基线入库前：公共 PDF Fixture 的来源、授权审核和二进制存储方式。
- P2 持久化实现前：Workspace 锁恢复、对象文件与 SQLite 提交/清理协议、备份一致性语义。
- P3 Replay 实现前：Run Manifest 和 Replay 首版采用 JSON Schema 还是代码生成 IDL。
- P4 性能门槛启用前：固定 CI 设备或独立基准设备，以及首发环境的性能预算。
