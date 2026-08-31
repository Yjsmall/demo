# ADR-0006：Annotation、Quote Anchor 与 Note Workspace v2

状态：Accepted

日期：2026-08-31

## 背景

P2 阅读闭环需要把文本选择转换为可恢复的高亮，并让关联 Markdown 笔记在自动保存时检测陈旧写入。页面像素不是稳定数据，完整笔记也不应嵌入高亮记录。

## 决策

- Workspace schema v2 新增 `annotations`、`annotation_quads` 和 `notes`，打开 schema v1 工作区时在单个事务内增量迁移。
- Annotation 绑定不可变 `DocumentVersionId`，保存页索引、PDF 页面坐标矩形、exact/prefix/suffix Quote Anchor、文本布局版本和有限颜色集合。
- Note 通过稳定 `AnnotationId` 关联高亮，保存 Markdown 原文和从 1 开始递增的 revision。更新必须提供当前 revision；陈旧 revision 返回 `conflict`。
- 删除 Annotation 时通过外键级联删除其坐标和 Note。Workspace 验证检查 SQLite 外键、缺失坐标和越界页索引，但不修复数据。
- Facade 和 Node-API 暴露同一组 DTO。Node-API 操作继续通过 async work 执行，Renderer 不接触 SQLite 或原生模块。

## 后果

- 高亮和笔记可以在 Workspace 关闭及 Utility Process 重启后恢复，UI 可以只保存稳定 ID 和 revision。
- 当前切片支持创建、列出和删除高亮，以及创建、更新和列出笔记；高亮几何更新和重新锚定留待后续需求验证。
- revision 只解决同一 Note 的陈旧写入，不替代 P3 的 Workspace 独占写入、崩溃恢复和跨进程并发控制。
