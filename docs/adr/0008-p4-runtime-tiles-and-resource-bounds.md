# ADR-0008：P4 Runtime、Tile 与资源边界

- 状态：已接受
- 日期：2026-09-01

## 背景

产品阅读热路径需要连续滚动、快速缩放与高 DPI Tile，同时 MuPDF document、structured text 和 display list 有严格的线程与生命周期约束。无界任务、旧缩放结果回写或跨 Electron 边界保留 native external Buffer，都会使关闭竞态、内存峰值和旧帧行为不可验证。

当前仍是开发阶段。Workspace 只承诺当前 schema v4，不维护旧 schema 的迁移链或迁移备份；任何非 v4 数据库均返回 `UNSUPPORTED_DOCUMENT`。

## 决策

Runtime 拥有有界优先级调度器。DocumentSession Actor 独占 document、structured text 与 display list 创建，任务优先级依次为可见 Tile、相邻页、缩略图和索引；等待满两秒的任务逐级提升。队列最多 256 项或 32 MiB，满载返回 `RESOURCE_EXHAUSTED`，取消只影响对应 Job。关闭先停止接收任务，再请求取消、排空回调，最后销毁 session、clone context 与 Runtime。

MuPDF 的原始 context/document 只在 Session Actor 上使用。并行栅格 worker 必须使用共享 locks 创建的 clone context，并只读取生命周期覆盖 Job 的 display list；禁止从多个线程同时使用同一个 context 或 document。Job 完成回调回到 Runtime executor，绝不在 MuPDF worker 上调用 Node-API。

Renderer 使用连续页面占位和 IntersectionObserver，只激活可见及邻近页。滚动、缩放、DPR 或渲染选项变化都会递增 generation 并取消旧 Tile Job；返回 generation 不匹配的结果不得写入画面。Tile 为最大 512x512 RGBA8。组合缩放最大 16，页面栅格单边最大 16384，总像素最大 134217728。

默认预算为 Tile LRU 256 MiB、文本索引 64 MiB、display list 16 页或 128 MiB、MuPDF 分配 256 MiB、跨边界在途 Buffer 64 MiB。超限返回 `RESOURCE_EXHAUSTED`，维度、格式和溢出返回 `INVALID_ARGUMENT`。

Utility 与 Renderer 使用 generation 绑定的专用 MessagePort 传送 Tile。Node-API 只创建普通 ArrayBuffer，将 native RGBA 复制一次；不使用 external ArrayBuffer。Electron 44.1 的 `MessagePortMain.postMessage` transfer 参数仅接受 `MessagePortMain[]`，不能转移 ArrayBuffer，因此该端点使用 structured clone，禁止传入无效 transfer list。若 Electron 后续提供 ArrayBuffer transfer，再以契约测试启用所有权转移。

## 后果

- `renderPage` 与整页 PNG 继续服务 Probe、缩略图和兼容契约，产品阅读热路径只调用 `renderTile`。
- Tile cache key 不含 generation，但包含文档版本、页、缩放/DPR 与像素区域；generation 只决定任务结果能否提交。
- 派生 `index.db`、Tile、display list 和日志均可删除重建，不进入 readerpkg。
- P4.1 验收必须覆盖优先级老化、队列满、取消、LRU 字节淘汰、极端页面拒绝与待处理任务关闭。
