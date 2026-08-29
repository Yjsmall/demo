# ADR-0001：Electron 首发与 Node-API 调用边界

状态：Accepted

日期：2026-08-29

## 背景

Context Reader 的业务规则由 C++ Reader Kernel 实现。首发产品确定为 Electron 桌面客户端，需要决定 `reader_node` 是直接调用 C++ Application Facade，还是先调用稳定 C ABI 再进入 Facade。

Kernel、Node-API Addon 和 Electron Utility Process 在首发形态中由同一仓库、同一工具链和同一发布流水线共同构建，不存在独立升级 Kernel 动态库的需求。未来原生客户端和其他语言嵌入需要一个不暴露 C++ 类型、异常和工具链 ABI 的稳定边界。

## 决策

首发调用链为：

```text
Electron Renderer
  -> typed IPC
Electron Utility Process
  -> reader_node (Node-API)
C++ Application Facade
  -> reader_core
```

`reader_node` 直接链接并调用 C++ Application Facade，不经过 C ABI。`reader_node` 与 `reader_core` 一起构建和发布，不承诺二者之间的二进制 ABI 稳定性。

C ABI 作为并列 Binding 保留，在出现第一个真实原生或跨语言 FFI 消费者时实现：

```text
Native / Other Language
  -> reader_c (stable C ABI)
C++ Application Facade
  -> reader_core
```

所有 C++ 目标统一使用 C++20 编译。已有 C++17 风格业务代码作为 C++20 兼容代码直接编译，不按模块混用语言标准。

## 理由

直接调用 Facade 更符合当前部署事实：Node Binding 与 Kernel 不独立发布，因此没有必要在进程内部为自己建立一层稳定二进制协议。它还保留了 C++ 值类型、`Result<T>`、类型化 Job 和 RAII 的表达力，减少 opaque handle、结构体版本、字符串和 Buffer 在两层 Binding 间的重复转换。

C ABI 的主要价值是编译器、语言和发布单元之间的稳定边界。没有真实消费者时提前冻结它，会把尚在演进的 PDF、Job、事件和 Buffer 模型固化为长期兼容承诺，并增加 Electron 首个闭环的实现和测试成本。

## 后果

- `reader_node` 必须保持为薄适配器，不能因为直连 C++ 就承载业务校验、SQL 或 MuPDF 调用。
- `reader_node` 或 `reader_core` 变化时二者一起重新构建和发布。
- Application Facade 与 Node-API 使用同一组行为场景，但仍分别测试 Node 参数转换、Promise/Event、线程切换和 Buffer 释放。
- 未来 C ABI 复用同一 Facade 和行为契约，但拥有独立的 ABI 生命周期与兼容测试。
- C++ exception 仍不得越过 Node-API、线程入口或未来 C ABI。

## 未采用方案：Node-API 包装 C ABI

这一方案可以强制 Electron 和原生消费者经过完全相同的二进制入口，并允许 Kernel 动态库与 Node Addon 独立更新。代价是 Electron 路径增加一层 C 数据模型、handle 生命周期和错误转换，且必须在产品模型尚未稳定时承担 ABI 兼容承诺。

当出现以下任一条件时可以重新评估：

- `reader_core` 需要作为独立动态库发布，并与 `reader_node` 分开升级。
- 多个 Binding 必须共享完全相同的低层 Buffer/Job 实现，重复维护已经成为可测量成本。
- 首发部署模型改变，Node Addon 不再与 Kernel 使用同一工具链和发布单元。

即使重新评估，C ABI 也不是进程隔离或安全沙箱；若需要更强隔离，应单独设计进程边界和 IPC 协议。
