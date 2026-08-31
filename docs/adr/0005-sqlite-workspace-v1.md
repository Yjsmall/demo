# ADR-0005：SQLite Workspace v1 与内容寻址对象仓库

状态：Accepted

日期：2026-08-31

## 背景

P2 首次引入需要跨进程重启恢复的权威数据。Workspace 必须保存稳定 ID、Document 当前版本与不可变 PDF 原文件，同时保持 SQLite、文件系统和 MuPDF 类型不进入公共 Facade DTO。首个纵向切片还需要提供只读不变量检查，使持久化错误在批注和笔记进入 schema 前即可被发现。

## 决策

- 锁定 SQLite 3.53.4 官方 amalgamation，并在 CMake FetchContent 中校验 SHA3-256；业务目标只通过 `SQLite::SQLite3` 链接。
- Workspace schema v1 使用 `STRICT` 表保存 metadata、documents 和 document_versions；`PRAGMA user_version` 是 migration 版本来源。
- PDF 原文件按 SHA-256 写入 `objects/pdf/<前两位>/<完整哈希>.pdf`。相同内容的重复导入复用既有 Document 与 DocumentVersion，不重复创建权威记录。
- 导入先用 PdfEngine 验证输入并获得页数，再写不可变对象，最后以单个 SQLite 事务插入 Document/Version 并激活版本。事务提交前失败不得产生数据库半成品。
- Application Facade 只暴露项目拥有的 DTO 与 `Result`；SQLite 连接、语句和 schema SQL 留在 adapter 内。
- `reader_node` 为每个 addon 环境持有一个 ReaderRuntime。Workspace、文档打开、页面渲染和文本提取操作通过 N-API async work 返回 Promise，并在 Application Facade 入口串行化；PNG 使用 Node Buffer 传递。
- 只读验证执行 SQLite quick check，并校验活动版本归属、对象路径、大小与 SHA-256。验证不得修复或删除数据。

## 后果

- Workspace 可在关闭 ReaderRuntime 后重新打开，稳定 ID、文档元数据和内容哈希保持不变。
- 重启后的文档通过稳定 DocumentId 解析到活动版本的不可变对象；Application Facade 拥有 DocumentSession 生命周期，Binding 不接触对象路径或 MuPDF 句柄。
- Electron Renderer 无需也不允许加载原生模块；当前 P2 验证路径在 Utility Process 内调用 `reader_node`。
- 对象文件先于数据库事务落盘，因此提交前进程终止可能留下未引用对象。P3 必须补充独占锁、孤立对象回收、迁移备份与故障注入；v1 验证当前只检查被引用对象。
- Electron 桌面产品应在 Main 层拒绝第二个应用实例；Workspace 独占锁仍作为 Kernel 的宿主无关单写者不变量存在，用于防御 Utility 重启重叠和维护工具冲突，而不是支持多个应用并发编辑。
- P2 已为导入、打开与渲染建立带稳定 ID 的可取消 Job：Renderer、Main、Utility Process 与 Node-API 传递同一 Job ID，内核取消令牌在导入哈希/复制、文档会话提交和渲染结果提交边界生效。P3 继续补充完整 Node-API Job 契约、进度、关闭竞态与 Replay。
- P2 的受控故障测试会在导入事务 `COMMIT` 前后终止独立 Utility Process，并验证重新打开后的文档计数与工作区不变量；孤立对象清理及其他写入/迁移故障矩阵仍属于 P3。
