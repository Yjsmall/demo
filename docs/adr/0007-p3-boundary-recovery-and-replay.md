# ADR-0007：P3 Utility 恢复、边界契约与 Replay

- 状态：已接受
- 日期：2026-08-31

## 背景

P2 已建立 Renderer -> Main -> Utility Process -> Node-API -> Application Facade 路径，但 Utility 异常退出会丢失进程内 Workspace/Document 状态，也缺少能稳定复现取消和关闭时序的工程证据。P3 还要求 Facade 与 Node-API 语义一致，并要求 Node 参数、错误、Job、取消、Buffer 和关闭行为具有可执行契约。

## 决策

Electron Main 是 Utility Process 的 supervisor。每次启动分配单调递增的 generation；请求与响应绑定 generation，进程退出时该 generation 的所有未完成请求以 `UTILITY_EXITED` 失败。Main 只保存恢复所需的 Workspace 路径和稳定 DocumentId，新 Utility ready 后先重新打开这些状态，再接受后续请求。旧 generation 的消息不会解析新 generation 的 Promise。

Node-API 使用真实 Electron Utility 执行契约测试。参数错误在 Binding 边界返回稳定 `INVALID_ARGUMENT`；业务错误沿用 Kernel 错误码；渲染结果使用独立 Buffer 拷贝。另由 Facade 契约探针和 Node 契约输出归一化 JSON，并逐字段比较相同行为场景。

Replay 首版采用 JSON Schema 描述的 JSONL v1。Manifest 固定 schema version、场景名和 Seed；Step 使用虚拟 Clock 的逻辑时间。受控 Executor 按逻辑时间和 Seed 决定执行顺序，并通过真实 Node-API Job/取消/关闭边界执行。P3 基线连续重放两次同一取消场景，要求输出 JSONL 字节完全一致且包含一个 `CANCELLED` Job 结果。

## 后果

- Utility 重启不要求 Renderer 重新创建 Workspace/Document 上下文，正在执行的旧请求则明确失败，由调用方决定是否重试。
- Main 不保存 MuPDF 句柄、SQLite 连接或对象路径；恢复仍通过稳定 Application Facade 操作完成。
- Replay 日志只包含逻辑操作、逻辑时间、Job ID 和稳定错误码，不记录用户文件路径、PDF 内容或笔记正文。
- P3 的统一验收入口为 `pixi run p3`；Replay 证据写入忽略版本控制的 `build/p3-replay/`。
- JSONL v1 不承诺跨主版本无限兼容；增加不兼容字段或语义时提升 schema version。
