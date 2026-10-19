# 验证记录

最新功能验证：2026-10-19，macOS arm64；执行者为 Codex。以下是本机实际运行结果，不是 GitHub Actions 或其他平台的结果。

| 范围 | 结果 |
| --- | --- |
| Debug 完整构建与测试 | 9/9 CTest，退出 0 |
| Release 完整构建与测试 | 9/9 CTest，退出 0 |
| ASan + UBSan 核心测试 | 6/6 CTest，退出 0 |
| 端到端保存与回放 | 10,000 条源数据、核对输出、正式文件和回放输出的字段与顺序一致 |
| 记录浏览核心 | 38 个检查，覆盖排序、状态、预览、刷新、限额和文件变化 |
| 浏览集成 | 详情、编号选择、返回列表/切换记录、连续命令、只读性、损坏/删除后重查、EOF 与信号退出 |

已使用的工具：Apple Clang 21.0.0、CMake/CTest 4.4.4、Python 3.13.3、Unix Makefiles。nlohmann/json 3.11.3 与 Boost 1.88.0 固定版本。没有验证工具最低版本兼容性、其他操作系统、TSan 或性能指标。

## 测试组成

6 个核心 CTest：version、numeric、frame_parser、numeric_worker、recording、recording_catalog。

3 个集成 CTest：TCP/CLI、保存回放、记录浏览。一个集成 CTest 内包含多个场景；检查次数、场景数与 CTest 项目数是不同计量，不应相互混用。

异常场景包括非法 JSON/类型/长度、半包断线、重复与缺失序号、容量不足、停止排空、聚合失败、录制提前 EOF、进程中断、损坏文件及文件写入失败。写入失败仅通过限制专用测试子进程的文件大小触发，不写满磁盘、不改变系统全局限额。

记录浏览测试还检查带空格、中文、引号、换行和 ESC 的文件名，拒绝符号链接与特殊文件，验证浏览前后文件哈希保持一致。

## 复现

按 [README](../README.md) 构建后运行完整测试。Release 可使用独立目录：

```sh
cmake -S . -B build/cmake-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSTREAMDESK_BUILD_SERVER=ON
cmake --build build/cmake-release --parallel 2
ctest --test-dir build/cmake-release --output-on-failure --no-tests=error
```

在支持 Clang Sanitizer 的环境中，单独验证核心：

```sh
cmake -S . -B build/asan-core -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSTREAMDESK_BUILD_SERVER=OFF -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/asan-core --parallel 2
ctest --test-dir build/asan-core --output-on-failure --no-tests=error
```

当前测试不要求恢复历史错误源码。NUL 缺陷的历史失败与修复过程见 [排错案例](debugging/nul_boundary.md)，修复后的回归已包含在源码测试中。

## 公开源码导出验证（2026-10-19）

发布前从 Git 暂存区导出仅包含拟公开文件的干净目录，重新进行 Debug 配置、构建与完整测试，9/9 CTest 通过，退出 0；一键保存回放演示也通过。服务端构建显式指向本机已有的 Boost 1.88.0 缓存，未依赖未公开的个人笔记或原构建目录中的目标文件。第三方头文件与许可证保持原始内容。

## 证据边界

本地原始运行日志、生成数据、依赖下载和二进制产物位于被忽略的 `build/`，不随仓库发布。源码中的测试与本页摘要可供重新验证；[源码 SHA-256 清单](core_source_manifest.sha256)记录功能验证对应的代码文件。发布后的版本也可通过 Git 提交定位。

通过测试不能证明所有输入、并发情形或平台均正确；核心 Sanitizer 结果也不能外推成所有 CLI 集成场景均经过 Sanitizer 验证。
