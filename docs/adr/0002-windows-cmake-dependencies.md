# ADR-0002：Windows x64、Pixi 工具环境与 CMake 依赖管理

状态：Accepted（首选编译器由 ADR-0004 部分取代）

日期：2026-08-29

## 背景

产品 v0.1 需要确定首发平台、开发工具环境、构建入口和第三方依赖获取方式。当前开发环境为 Windows x64。Scoop 已独立安装 MSYS2 2026-06-11 和 Pixi 0.78.0，独立 MSYS2 的 UCRT64 环境提供 GCC/G++ 16.2.0。随 Ruby 安装的另一套 MSYS2 不进入项目工具链。项目尚未形成需要兼容的 vcpkg、Conan 或其他包管理清单。

Pixi 提供 Windows 环境、lockfile 和任务管理，适合锁定 CMake、Ninja 等开发工具。CMake 本身不是通用二进制包管理器，但它提供 `FetchContent`、`add_subdirectory` 和 `ExternalProject`，足以在首个版本中统一编排源码依赖的获取、构建和链接。项目仍需显式维护版本、来源、完整性和许可证元数据。

## 决策

- 产品 v0.1 首发平台为 Windows x64。
- Scoop 只作为开发机引导安装器，用于安装 Pixi 和独立 MSYS2；Scoop 清单不定义项目构建环境。
- Pixi 是开发工具环境与任务入口，通过提交到仓库的 `pixi.toml` 和 `pixi.lock` 锁定工具版本。
- CMake 3.28 是项目最低构建版本，也是开发、CI 和发布的唯一构建入口；Ninja 是首选生成器。
- 独立 MSYS2 UCRT64 GCC 16.2.0 是产品 v0.1 的首发编译器。随 Ruby 安装的 MSYS2 不得进入项目 preset 或 CI。
- 不引入 vcpkg 或 Conan。
- 原生支持 CMake 的源码依赖优先通过 `FetchContent` 获取，并通过 target 使用要求接入。
- 仓库内依赖或已经取得的源码树使用 `add_subdirectory` 接入。
- 不提供原生 CMake 构建的依赖可以使用 `ExternalProject` 编排，并封装为项目拥有的 imported target。
- 每项依赖锁定明确版本、不可变下载地址或提交 ID，以及 SHA-256 或等价完整性信息。浮动分支和未固定标签不能进入 CI 与发布构建。
- 依赖清单同时记录许可证、补丁和构建选项。镜像或离线源可以替换下载位置，但不能静默改变内容。
- Pixi/conda 包只提供开发工具，不直接作为 MSYS2 UCRT64 GCC 的链接时 C++ 库；参与链接的 C/C++ 依赖必须由同一工具链构建或经过明确 ABI 验证。

## 后果

- P0 维护一份 Pixi lockfile、一套 CMake preset、源码依赖清单和 target 图，不需要同步多个 C++ 包管理器。
- MSYS2 `pacman` 只用于安装和更新 UCRT64 编译器等工具链组件，不作为项目 C++ 库的依赖管理器，也不能替代 Pixi/CMake 锁定信息。
- Windows x64 之外的平台不进入 v0.1 发布矩阵，但公共 C++ 代码仍避免不必要的平台绑定。
- MuPDF 未原生采用本项目的 CMake target 约定，P0 必须验证其 Windows 构建、补丁和 imported target 封装后才能进入 P1。
- SQLite、测试框架和其他依赖也必须通过命名 target 使用，业务代码不能依赖下载目录或机器路径。
- Pixi 和 CMake 编排会增加首次环境创建与配置时间；CI 应缓存工具环境和源码下载，但发布产物必须能由锁定信息重新构建。

## 后续验证

- 后续 Electron major 升级必须重新运行 `.node` 构建、PE 导入边界和 Utility Process 加载测试。
- MuPDF 1.28.3 的 Windows 构建命令、提交锁定和 CMake imported target 已由 ADR-0003 的 2026-08-29 验证记录落实。

P0 的具体版本、MinGW import library 处理和验证结果见 ADR-0003。
