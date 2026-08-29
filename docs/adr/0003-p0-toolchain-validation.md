# ADR-0003：P0 工具链验收与 MuPDF 商业许可

状态：Accepted

日期：2026-08-29

## 背景

ADR-0002 选择 Windows x64、Pixi、CMake 和独立 MSYS2 UCRT64，但仍需通过真实 Electron 原生模块证明 GCC 工具链可用。P0 同时要求消除 MuPDF 许可阻塞，并锁定首发 Electron/Node 构建基线。

## 决策

- 产品 v0.1 使用已经取得的 MuPDF 商业许可。许可证文本、凭据和商业发行包不提交到公共源码仓库。
- 首发 Electron 锁定为 44.0.0，其内置 Node.js 为 24.18.1；CMake.js 锁定为 8.0.0。
- C++ 编译器使用独立 MSYS2 UCRT64 GCC/G++ 16.2.0。
- Pixi lockfile 当前解析为 CMake 3.31.8、Ninja 1.13.2 和开发用 Node.js 24.19.0。开发 Node 只运行构建工具，不替代 Electron 内置 Node。
- Node-API 绑定使用 N-API 8，并直接链接同一工具链构建的 `reader_core`。
- MinGW 不使用 CMake.js 注入的 MSVC `/DELAYLOAD:NODE.EXE`。构建通过 `dlltool` 从受审查的 N-API 导出清单生成以 `electron.exe` 为目标的 GNU import library。
- CI 和本地验证必须拒绝导入 `node.exe` 的 `.node` 文件，并要求实际 Electron Utility Process 成功加载模块。

## 验证证据

本地 `pixi run p0` 已完成以下验证：

- GCC 16.2.0 使用 CMake preset 构建 `reader_core` 和测试可执行文件。
- CTest 的 `reader_core_test` 通过。
- `reader_node.node` 的 PE 导入表包含 `electron.exe`，不包含 `node.exe`。
- Electron 44.0.0 Main Process 创建 Utility Process，Utility Process 加载 `reader_node` 并返回 Kernel 版本 0.1.0 与 Application API version 1。
- npm 锁定依赖安装后的审计结果为 0 个已知漏洞。

远程 GitHub Actions 工作流已经建立。首次推送后的运行结果需要作为 P0 的远程 CI 证据保存。

## 后果

- UCRT64 GCC 被接受为 Windows x64 首发工具链，不再把 MSVC/clang-cl 作为 P0 阻塞项。
- 新增 N-API 函数时必须同步更新 `electron_napi.def`；导入边界检查会防止静默退回 `node.exe`。
- Electron major、CMake.js 或 GCC major 升级必须重新执行完整 P0 验证。
- `reader_node` 当前仍依赖 UCRT64 `libwinpthread-1.dll`。P2 打包前必须选择随应用分发该运行库或改为经过验证的静态链接方案。
- P1 可以开始 MuPDF 构建验证，但仍需确认商业发行包的版本、补丁方式和 imported target 封装。
