# ADR-0004：Clang 优先的 UCRT64 工具链

状态：Accepted

日期：2026-08-29

## 背景

ADR-0002 和 ADR-0003 以 MSYS2 UCRT64 GCC 16.2.0 验证了 Windows x64、Electron Node-API 和 CMake 构建边界。项目后续开发希望优先使用 Clang 的诊断能力，同时保留已经验证的 GCC 路径作为本地恢复手段。

## 决策

- Windows x64 首选独立 MSYS2 UCRT64 Clang/Clang++ 22.1.8，并使用同版本的 lld 链接。
- `CONTEXT_READER_COMPILER` 接受 `auto`、`clang` 或 `gcc`；默认 `auto`，检测到 Clang 时选择 Clang，否则回退到 GCC。
- CI 同时安装 UCRT64 Clang、lld 和 GCC，但默认路径必须实际使用 Clang/lld；`toolchain-check` 验证选中编译器、链接器及其版本。
- GCC 16.2.0 只作为显式或缺少 Clang 时的回退，不再是首选编译器。
- 两种编译器继续使用同一个 UCRT64 sysroot、CMake preset、Ninja 生成器和 Electron N-API import library 方案。

本 ADR 仅取代 ADR-0002 和 ADR-0003 中关于首选编译器的决策；其他决策继续有效。

## 后果

- 开发机安装 UCRT64 Clang 后，无需修改 preset 即自动切换到 Clang；需要复现 GCC 行为时设置 `CONTEXT_READER_COMPILER=gcc`。
- Pixi 配置任务使用 CMake `--fresh`，避免在 Clang/GCC 切换时保留另一编译器产生的缓存状态。
- CMake 缓存记录 `CONTEXT_READER_SELECTED_COMPILER`，构建日志必须能够说明实际选择。
- Clang 和 GCC 路径都必须维持 `reader_core` 测试、PE import boundary 和 Electron Utility Process 加载能力。
- 编译器版本升级需要同步更新版本门槛并重新运行完整 `pixi run p0`。
