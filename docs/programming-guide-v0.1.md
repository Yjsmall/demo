# Context Reader 编程指导 V0.1

状态：Draft

语言基线：C++20

核心风格：Composition First

## 1. 指导原则

本项目优先使用组合表达系统结构：

```text
Kernel 由模块组合
模块由用例组合
用例由 Port 组合
Adapter 实现 Port
Composition Root 组装运行环境
```

继承仅用于稳定的抽象接口和必要的平台框架接入，不用于共享业务实现。

## 2. 基本约束

- 领域代码不得依赖 Electron、Node、HTTP、SQLite 或 MuPDF。
- 业务规则不得出现在 Binding、Controller、数据库 Mapper 或 UI 中。
- 所有外部依赖通过构造函数注入。
- 禁止 Service Locator 和业务全局单例。
- 模块不得直接访问其他模块的数据库表或内部类。
- 新增抽象必须消除真实重复、形成替换边界或隔离第三方依赖。
- 不为“以后可能需要”预先建立继承层次和通用框架。

### 2.1 C++ 语言标准

- Kernel、Application Facade、Adapter 和一方 C++ Binding 统一使用 C++20 编译。
- 已有仅使用 C++17 特性的实现可以直接作为 C++20 兼容代码纳入，不需要改写成新语法。
- 不允许业务模块分别选择 C++17/C++20；混合语言标准会扩大模板、标准库和构建配置的兼容面。
- 公共头文件避免依赖非标准编译器扩展。未来 C ABI 用于隔离其他语言和独立工具链，不用于降低 Kernel 内部语言基线。

### 2.2 构建与依赖

- 首发构建目标为 Windows x64，CMake 最低版本为 3.28。
- Scoop 只负责在开发机上引导安装 Pixi 和独立 MSYS2，不定义项目构建结果。
- Pixi 通过 `pixi.toml` 和 `pixi.lock` 锁定 CMake、Ninja 等开发工具，并作为本地与 CI 的统一任务入口。
- CMake 是唯一构建入口，并负责源码依赖获取和构建编排；项目不同时维护 vcpkg 或 Conan 清单。
- MSYS2 `pacman` 只提供 UCRT64 编译器和必要的工具链组件。Clang 是首选，GCC 是回退；Pixi/conda 包不能直接作为 UCRT64 工具链的链接时 C++ 库，避免混用不兼容 ABI。
- 原生支持 CMake 的源码依赖优先通过 `FetchContent` 获取并使用 `add_subdirectory` 集成。
- 不提供原生 CMake 构建的依赖可以通过 `ExternalProject` 编排，但必须封装为项目拥有的 imported target，不能向业务 target 泄漏路径和平台参数。
- 每项依赖必须锁定明确版本、不可变来源和 SHA-256 或等价完整性信息；禁止默认跟踪分支或浮动标签。
- 开发、CI 和发布必须通过同一份 Pixi lockfile、CMake preset 和源码依赖锁定清单构建。离线或镜像需求通过可配置下载源满足，不改变依赖版本。

## 3. 推荐代码结构

```text
kernel/
  include/context_reader/
    application/        稳定 Application Facade 与公共 DTO
  src/
    shared/domain/      ID、Revision、Result 等少量稳定共享类型
    application/
      coordinators/     跨模块同步编排
      facade/           Facade 实现
    modules/
      workspace/
        domain/
        application/
        ports/
      document/
        domain/
        application/
        ports/
      pdf/
        domain/
        application/
        ports/
      annotation/
      note/
      asset/
      search/
      job/
    adapters/
      mupdf/
      sqlite/
      filesystem/
    runtime/            Composition Root、Executor、生命周期
bindings/
  node/                 首发 Binding，直接调用 Application Facade
  c/                    真实 FFI 消费者出现后引入
hosts/
  electron/             首发 Host
  cli/                  后续 Host
  daemon/               后续 Host
tests/
  unit/
  contract/
  integration/
  fixtures/
tools/
  probe/
  fixture/
  replay/
  bench/
  abi-check/
  workspace/
contracts/
  diagnostic/
  replay/
  run-manifest/
baselines/
```

源码采用“模块优先、模块内分层”。一个业务概念的领域规则、用例和 Port 保持在同一模块目录；只有跨模块 Coordinator、Facade 和真正稳定的共享值类型位于公共目录。Adapter 实现模块拥有的 Port，但第三方类型不能进入模块公共头文件。

公共头文件与内部实现分离。每个模块对应明确的 CMake target 或受检查的 target 集，只暴露供 Facade/Coordinator 使用的最小 surface。模块不得通过 include 路径访问另一个模块的 `domain/` 或 `application/` 私有目录。

## 4. 组合方式

### 4.1 用例对象

用例是小而明确的可调用对象：

```cpp
class CreateAnnotation final {
public:
    CreateAnnotation(
        AnnotationRepository& annotations,
        DocumentQueries& documents,
        UnitOfWork& unit_of_work,
        Clock& clock)
        : annotations_(annotations),
          documents_(documents),
          unit_of_work_(unit_of_work),
          clock_(clock) {}

    Result<Annotation> execute(const CreateAnnotationCommand& command);

private:
    AnnotationRepository& annotations_;
    DocumentQueries& documents_;
    UnitOfWork& unit_of_work_;
    Clock& clock_;
};
```

用例不自行查找依赖，也不读取全局 Runtime。

### 4.2 Composition Root

对象创建和具体实现选择集中在 Composition Root：

```cpp
class ReaderRuntime final {
public:
    static Result<std::unique_ptr<ReaderRuntime>> create(
        const RuntimeOptions& options);

    ReaderApplication& application() noexcept;

private:
    ThreadPool executor_;
    SqliteDatabase database_;
    ContentAddressedAssetStore assets_;
    MuPdfEngine pdf_engine_;

    DocumentModule documents_;
    AnnotationModule annotations_;
    NoteModule notes_;
    ReaderApplication application_;
};
```

业务代码中不允许随意 `new` Repository、数据库连接或线程池。

### 4.3 装饰器

横切能力通过组合包装，而不是基类钩子：

```text
MuPdfEngine
  -> CachedPdfEngine
  -> MeasuredPdfEngine
```

只有确实需要替换或测试的边界才定义 Port。不要给每个类机械地创建 interface。

### 4.4 流程组合

跨模块流程由 Coordinator 显式编排：

```text
ValidateImport
  -> HashDocument
  -> StoreObject
  -> CreateDocumentVersion
  -> ScheduleTextIndex
```

不要用隐藏事件链完成必须同步成功或失败的核心事务。

## 5. 继承使用规则

允许：

- Port 的纯虚接口。
- 平台或测试框架要求的继承。
- 有清晰 substitutability 的少量运行时多态。

禁止或强烈不建议：

```text
BaseService
BaseManager
BaseModule
AbstractReaderController
AdvancedPdfService : BasePdfService
```

接口示例：

```cpp
class PdfEngine {
public:
    virtual ~PdfEngine() = default;
    virtual Result<DocumentHandle> open(const ObjectKey& key) = 0;
    virtual JobHandle render(const RenderRequest& request) = 0;
};
```

接口不提供 protected 状态和部分默认业务实现。

## 6. 值类型与领域类型

优先使用能在构造时保持有效的值对象：

```cpp
class PageIndex final { /* non-negative invariant */ };
class Revision final { /* monotonic value */ };
class DocumentId final { /* opaque identifier */ };
class PdfPoint final { /* finite coordinates */ };
```

避免在整个系统中传递无语义的 `std::string`、`int` 和裸 `double`。

页面坐标、屏幕坐标、像素坐标和缩放比例必须是不同类型，禁止隐式混用。

DTO 用于边界传输，领域对象用于维护规则。不要直接把 C ABI struct 或数据库 row 当成领域对象。

## 7. 所有权与生命周期

采用以下默认规则：

- 值语义优先。
- 独占所有权使用 `std::unique_ptr`。
- 共享所有权只有在生命周期确实共享时使用 `std::shared_ptr`。
- 非拥有且必需的依赖使用引用。
- 非拥有且可选的依赖使用指针或明确的 optional reference abstraction。
- 连续只读数据使用 `std::span<const T>`。
- 不返回指向临时对象、容器内部或 MuPDF 内存的悬空 view。

`shared_ptr` 不能作为避免思考所有权的默认答案。出现循环所有权即视为设计问题。

## 8. 错误处理

可预期失败使用 `Result<T>`：

```text
InvalidArgument
NotFound
AlreadyExists
Conflict
WorkspaceBusy
UnsupportedDocument
PasswordRequired
Cancelled
ResourceExhausted
StorageFailure
PdfFailure
Internal
```

规则：

- Adapter 捕获第三方库异常并转换为稳定错误。
- C++ exception 不得穿越 C ABI、Node-API 或线程入口。
- 错误码稳定；展示文本可以本地化和调整。
- 日志记录内部诊断，返回客户端的信息避免泄露路径和敏感数据。
- Programmer error 使用断言或 fail-fast，不伪装成普通业务错误。

若 C++20 标准库缺少 `std::expected`，项目应采用一个受控的 expected 实现并统一封装为 `Result<T>`，不得混用多种错误模型。

## 9. 并发与异步

### 9.1 Runtime 拥有线程

线程池、调度器和定时器由 Runtime 统一创建和关闭。模块不得为每个请求无限创建线程。

### 9.2 DocumentSession Actor

MuPDF 文档句柄由 DocumentSession 单独拥有。外部通过消息或序列化队列请求操作，不共享裸 document 指针。Session 命令必须有明确的输入、响应、取消和关闭语义；Actor 不得通过持有 Application 全局锁来模拟队列，也不得让 Binding 自行决定 MuPDF 的线程安全规则。

页面信息、structured text index 和 display list 属于 Session 派生缓存。缓存对象的线程归属必须由类型或封装表达；只有经过 MuPDF 线程规则验证的不可变对象才能交给 Runtime Executor 栅格化。Actor、Runtime 线程池和宿主异步队列必须各自职责单一，避免形成多层同步等待。

### 9.3 长任务

导入、打开文档、渲染、索引、OCR 和导出返回 Job Handle：

```cpp
JobHandle job = application.render_page(request);
job.cancel();
job.on_complete(executor, callback);
```

回调不得在未知第三方线程直接进入 UI 或 FFI。Binding 负责切换到宿主指定的 Dispatcher。

### 9.4 锁

- 锁保护最小状态，不包围慢速 I/O 或回调。
- 明确全局锁顺序。
- 使用 RAII lock guard。
- 不在持锁状态下发布事件或调用插件。
- 优先使用 actor/strand 所有权减少共享可变状态。

## 10. 数据库规则

- SQL 只能存在于 SQLite Adapter。
- Migration 只前进，不修改已经发布的历史 migration。
- 每次 migration 在真实旧版本数据库副本上测试。
- 用例定义事务边界，Repository 不自行提交半个业务操作。
- 模块只访问自己拥有的表。
- 派生索引必须有明确的重建命令和版本标记。
- 禁止插件和客户端直接打开 Workspace 数据库。

删除业务数据时必须先定义语义：级联删除、软删除、保留孤立笔记还是转移所有权，不能只依赖数据库默认行为。

## 11. C ABI 指导

C ABI 不进入 Electron 首发路径。在出现原生或跨语言 FFI 消费者后，它作为 Application Facade 的独立 Binding 实现；以下规则从首次公开 ABI 起生效。

禁止从 C ABI 暴露：

```text
std::string
std::vector
std::shared_ptr
template 类型
C++ exception
MuPDF / SQLite 类型
```

推荐形式：

```c
typedef struct rk_runtime rk_runtime;
typedef struct rk_job rk_job;

typedef struct rk_buffer {
    const uint8_t* data;
    size_t size;
    void* owner;
} rk_buffer;

rk_status rk_runtime_create(
    const rk_runtime_options* options,
    rk_runtime** output);

rk_status rk_render_submit(
    rk_runtime* runtime,
    const rk_render_request* request,
    rk_job** output);

rk_status rk_job_take_buffer(rk_job* job, rk_buffer* output);
void rk_buffer_release(rk_buffer* buffer);
void rk_job_cancel(rk_job* job);
void rk_runtime_destroy(rk_runtime* runtime);
```

每个函数都要记录线程安全、所有权、失败时输出参数状态和版本兼容规则。

## 12. Node-API 指导

- Node-API 绑定是 adapter，不是业务服务。
- `reader_node` 与 `reader_core` 使用同一 C++20 工具链构建，直接调用 C++ Application Facade，不经过 C ABI。
- `reader_node` 与 `reader_core` 一起发布，不承诺二者之间的二进制 ABI 稳定性。
- 不直接访问 SQLite 或 MuPDF。
- 参数先做结构和范围校验，再转换成 Application Command。
- 同步函数只处理极短操作；重任务返回 Promise。
- Pixel Buffer 尽量通过 external ArrayBuffer 转交，并提供唯一释放路径。
- Utility Process 退出时先取消 Job，再关闭 Runtime。
- Renderer 只接触经过 preload 限定的 typed API。

## 13. 模块通信

模块间优先级：

1. 同一业务事务使用显式 Coordinator。
2. 查询使用只读 Port。
3. 提交后的非关键副作用使用 Application Event。
4. 插件通知使用稳定 Client Event。

禁止通过全局 Event Bus 隐藏所有控制流。事件消费者必须幂等，并定义失败、重试和顺序语义。

## 14. API 与命名

- 类型使用名词：`DocumentRepository`、`AnnotationAnchor`。
- 用例使用动词：`ImportDocument`、`CreateAnnotation`。
- 布尔值表达问题：`is_encrypted`、`has_text_layer`。
- 避免 `Manager`、`Helper`、`Utils`、`Common` 等含义宽泛的名称。
- 单位进入名称或类型：`timeout_ms`、`PixelWidth`、`PdfPoint`。
- 缩写保持一致：`Pdf`、`Utf8`、`Id`，不混用多种大小写。

公共 API 名称一旦发布即视为兼容性承诺。

## 15. 头文件与依赖

- 头文件只包含声明所需依赖，优先前置声明。
- 不在公共头文件引入 MuPDF、SQLite、Node 或平台 SDK 头文件。
- 避免宏；必须使用时限制在 adapter 内并立即取消定义。
- `using namespace` 不得出现在头文件。
- 编译目标按模块划分，禁止形成一个可以任意 include 的巨型 target。
- 构建系统应检测并拒绝反向依赖和循环依赖。

## 16. 日志与隐私

使用结构化日志，不拼接难以查询的长字符串。日志记录操作和错误码，不默认记录：

- 笔记正文。
- 选中文字。
- PDF 内容。
- 用户名和绝对路径。
- 插件密钥和认证令牌。

日志调用不能改变业务行为，也不能成为错误处理的替代品。

## 17. 测试指导

每个新用例至少包含：

- 正常路径。
- 参数边界。
- 依赖失败。
- 并发或 revision 冲突（适用时）。
- 取消和资源释放（异步任务适用时）。

每个 Adapter 包含真实集成测试。Mock 只用于验证领域和用例，不用于证明 MuPDF、SQLite 或 ABI 实际可用。

修复 PDF 坐标、文本布局或 ABI 问题时，必须先将触发问题的输入加入回归语料。

验证工具本身也属于产品级代码：必须有稳定退出码、版本化输出 schema、错误测试和最小契约测试。工具默认调用 Application Facade 或已经实现的 Binding，不能为了测试方便绕过正式边界。

## 18. 性能指导

- 先测量，再优化。
- 热路径避免重复分配、字符串转换和像素复制。
- 控制面代码优先可读性，不为微小调用开销牺牲领域模型。
- 页面使用 display list + 可见 Tile 和可取消任务，整页 PNG 不作为默认阅读数据面。
- 每次像素分配前检查单边尺寸、总像素、字节数和整数溢出；缩放上限不能替代资源预算。
- Cache 必须有明确的字节上限、淘汰策略和 key version，不能只限制条目数量。
- 视口和缩放请求携带单调递增 generation；过期结果在提交前丢弃，并在 Tile 边界停止后续工作。
- structured text 缓存保留字符范围和 page-space Quad；行矩形不能作为精确选择的唯一数据。
- 优化提交包含基准、设备、PDF 样本和前后结果。
- 基准产物必须记录 Kernel、MuPDF、构建 ID、平台、文档哈希、命令和随机种子。

## 19. 注释与文档

注释解释原因、约束和不明显的线程/所有权规则，不复述代码。

以下变化必须更新文档或 ADR：

- 新的模块依赖。
- 公共 ABI/API 变化。
- 数据权威来源变化。
- 新增第三方运行时或插件权限。
- 并发模型变化。
- 无法轻易撤销的技术选择。

## 20. AI 辅助开发约束

AI 生成代码必须遵守与人工代码相同的边界：

1. 修改前确认功能属于哪个模块和用例。
2. 优先复用现有 Port 和值类型，不创建平行抽象。
3. 不把业务逻辑放进 Binding、Controller 或 UI。
4. 不修改已发布 migration 和 ABI 字段含义。
5. 不为了消除少量重复引入通用框架。
6. 提交包含对应测试，并运行受影响层级的验证。
7. 生成文件由工具生成，不手工维护两份契约。
8. 大范围重构与功能变化分开提交。

## 21. Code Review 检查表

- 功能是否进入正确模块？
- 是否通过 Application Facade 调用？
- 是否出现 Service Locator、全局状态或不必要继承？
- 对象所有权和线程归属是否明确？
- 是否泄漏 MuPDF、SQLite、Node 或平台类型？
- 事务和 revision 语义是否完整？
- 错误码是否稳定且不泄露敏感信息？
- Buffer 和 Job 在成功、失败、取消路径是否都释放？
- PDF 像素、Tile、缓存和跨边界 Buffer 是否具有按字节计算的上限？
- 过期 generation 是否可能覆盖当前页面，取消是否只能在整页完成后生效？
- 文本选择是否来自字符级 page-space 几何，而不是 DOM 行相交结果？
- 是否覆盖真实回归输入和边界测试？
- 缺陷是否能够通过 Fixture、Probe 或 Replay 产物稳定重现？
- 公共契约、迁移或 ADR 是否需要同步更新？

## 22. 完成定义

一个功能只有同时满足以下条件才算完成：

- 业务规则位于 Kernel 的正确用例中。
- 至少一个 Binding 能通过公共契约调用它。
- 单元测试和必要的 Adapter/契约测试通过。
- 错误、取消、资源释放和并发行为已定义。
- 没有新增未受控全局状态、跨模块 SQL 或第三方类型泄漏。
- 对性能敏感的功能满足已定义预算。
- 原生边界或 PDF 行为变化具有可重现的机器可读验证产物。
- 用户数据格式或公共 API 变化已文档化并可迁移。
