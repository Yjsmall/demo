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
- `reader_node` 为每个 addon 环境持有一个 ReaderRuntime。Workspace 操作通过 N-API async work 返回 Promise，并在 Application Facade 入口串行化。
- 只读验证执行 SQLite quick check，并校验活动版本归属、对象路径、大小与 SHA-256。验证不得修复或删除数据。

## 后果

- Workspace 可在关闭 ReaderRuntime 后重新打开，稳定 ID、文档元数据和内容哈希保持不变。
- Electron Renderer 无需也不允许加载原生模块；当前 P2 验证路径在 Utility Process 内调用 `reader_node`。
- 对象文件先于数据库事务落盘，因此提交前进程终止可能留下未引用对象。P3 必须补充独占锁、孤立对象回收、迁移备份与故障注入；v1 验证当前只检查被引用对象。
- 当前异步 API 防止 Node 主线程执行数据库与 PDF 工作，但尚未形成可取消 JobHandle；可取消导入、打开与渲染仍是 P2 的未完成退出条件。
