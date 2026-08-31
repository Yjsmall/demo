# 第三方依赖清单 V0.1

本清单记录参与源码构建的直接依赖。版本、来源和完整性信息必须与构建文件或包管理器锁文件一致；依赖类型不得越过这里定义的架构边界。

| 依赖 | 锁定版本 | 来源与完整性 | 许可证 | 使用边界 |
| --- | --- | --- | --- | --- |
| standalone Asio | 1.38.2 | GitHub tag `asio-1-38-2`；SHA-256 `9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db` | BSL-1.0 | `reader_core` 私有运行时执行器；公共接口只暴露标准库类型 |
| Catch2 | 3.15.0 | GitHub tag `v3.15.0`；SHA-256 `9650c55e497759cc39b977e45524bc8acb15256061c112080916ab6cb0b1ea66` | BSL-1.0 | 仅测试 target；当前用于 `reader_core_test` 与 CTest 测试发现 |
| node-addon-api | 8.9.2 | npm registry；SHA-512 integrity 由 `package-lock.json` 锁定 | MIT | 仅 `reader_node`；不得进入 Kernel 或公共 C++ 头文件 |

Asio 与 Catch2 由 CMake `FetchContent` 获取并校验 SHA-256。node-addon-api 由 `npm ci` 按 `package-lock.json` 安装；原生模块配置前会检查其头文件存在。所有 C++ target 继续使用首选 Clang、回退 GCC 的同一套 MSYS2 UCRT64 工具链。
