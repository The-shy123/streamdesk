# StreamDesk

**基于 C++17 的数值数据接入、保存与回放工具。**

StreamDesk 将 TCP 数据接入、协议校验、有界异步处理、录制文件和本地回放串成可运行的命令行流程。保存后的记录可以按编号浏览、查看和选择回放。

```text
Python 模拟源 → TCP → 增量拆包与字段校验 → 有界异步处理
                                           ↓
                                      录制文件 .sdr
                                           ↓
                          记录列表 → 查看详情 → 选择回放
```

## 功能

- **数据接入**：Boost.Asio 单连接回环 TCP；4 字节网络序长度前缀，支持任意分块、粘包和半包检测。
- **独立核心库**：numeric v1 编解码与校验；字符串所有权明确，序号使用精确的 `uint64_t` 转换。
- **有界异步处理**：任务条数与规范化 JSON 字节双限额；过载限时重试、异常统计、排空停止和线程回收。
- **保存与完整性校验**：指定采集条数，记录版本、流信息、序号范围和校验尾；未完成文件保留为 `.partial`，成功收尾后才发布，不覆盖已有文件。
- **记录浏览**：列表、详情、最多 5 条预览、编号选择、显式刷新；选中时重新验证文件。
- **回放控制**：顺序播放、单步、暂停、继续、停止和进度；停止或完成后可以返回列表选择另一条记录。

当前实测平台为 **macOS arm64**。文件操作和终端输入使用 POSIX 接口，其他平台尚未验证。当前是命令行工具，没有 GUI、HTTP 服务或数据库。

## 构建与测试

需要 C++17 编译器、CMake/CTest、Make 和 Python 3.9+。已验证工具版本与测试范围见 [验证记录](docs/validation.md)。

```sh
git clone https://github.com/The-shy123/streamdesk.git
cd streamdesk

# 校验并提取固定版本 Boost，仅写入项目 build/，不安装系统软件
python3 tools/fetch_boost.py

cmake -S . -B build/cmake-debug -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSTREAMDESK_BUILD_SERVER=ON
cmake --build build/cmake-debug --parallel 2
ctest --test-dir build/cmake-debug --output-on-failure --no-tests=error
```

完整构建包含 **6 项核心测试和 3 项集成测试**。测试包括 10,000 条数据的源端、保存文件与回放输出逐字段/顺序核对，以及非法输入、断线、停止、写入失败、损坏文件和记录浏览控制。

仅构建核心与本地工具时，无需 Boost 或网络服务：

```sh
cmake -S . -B build/core -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSTREAMDESK_BUILD_SERVER=OFF
cmake --build build/core --parallel 2
ctest --test-dir build/core --output-on-failure --no-tests=error
```

核心依赖的 nlohmann/json 已固定在源码中。服务端使用 Boost 1.88.0；首次下载约 137 MiB，可通过 `-DSTREAMDESK_BOOST_ROOT=/path/to/boost_1_88_0` 指向已有的同版本头文件。来源、哈希与许可证见 [依赖说明](docs/dependencies.md)。

## 一键演示

完成包含服务端的构建后运行：

```sh
python3 tools/demo.py --record
```

演示会启动本机服务、发送 1,000 条固定记录、保存正式文件、逐字段核对并执行回放。成功时打印 `PASS`、录制文件路径和浏览命令；日志与数据保存在独立的 `build/demo-*` 目录，不覆盖以前的结果。

示例结果：`accepted=processed=1000`、`failed=invalid=0`、`sum=249750`。这是功能核对，不是吞吐或延迟测量。

## 浏览记录并选择回放

```sh
./build/cmake-debug/streamdesk_records build --interactive
```

默认查看指定目录及下一层子目录，能够发现演示生成的记录。按实际编号操作：

| 命令 | 功能 |
| --- | --- |
| `list` | 显示当前记录列表 |
| `show 1` | 查看第 1 条记录的元数据与样例 |
| `replay 1` | 进入第 1 条记录的回放，初始暂停 |
| `refresh` | 重新发现记录并显示新的编号 |
| `quit` | 退出记录浏览器 |

回放中可输入 `next`、`play`、`pause`、`resume`、`status`、`stop`。停止或完成后返回列表；Ctrl+C 退出当前程序。完整用法见 [记录浏览](docs/records.md)。

## 手动录制与独立回放

终端 A，在项目根目录执行：

```sh
capture_dir=$(mktemp -d "$PWD/build/capture.XXXXXX")
./build/cmake-debug/apps/server/streamdesk_server --port 19090 --deadline-seconds 120 --record "$capture_dir/sample.sdr" --record-count 1000
```

看到 `READY` 后，终端 B 发送数据：

```sh
python3 tools/numeric_source.py --port 19090 --count 1000 --fragment 17
```

录制成功后，在原终端 A 回放：

```sh
./build/cmake-debug/streamdesk_replay "$capture_dir/sample.sdr" --interactive --interval-ms 100
```

指定条数、连续序号和正常收尾共同确定录制完成；提前断线、主动停止或写入失败不会生成已完成记录。`--output` 产生的 JSONL 仅用于核对，不能替代正式 `.sdr` 格式。详见 [保存与回放](docs/recording.md)。

## 代码与文档

| 位置 | 内容 |
| --- | --- |
| `libs/streamcore/` | 数据模型、解析、工作线程、录制与目录浏览核心及单元测试 |
| `apps/server/` | Boost.Asio TCP 服务 |
| `apps/cli/` | 浏览与独立回放共用的命令输入和播放逻辑 |
| `examples/` | 检查、回放、记录浏览等可执行程序 |
| `tests/integration/` | TCP、保存回放与浏览交互集成测试 |
| `tools/` | 固定数据源、依赖下载、演示与验证工具 |

- [架构与生命周期](docs/architecture.md)
- [数据协议](docs/protocol.md)
- [保存格式与回放控制](docs/recording.md)
- [记录列表与详情](docs/records.md)
- [验证结果与复现命令](docs/validation.md)
- [排错案例：NUL 尾部漏检](docs/debugging/nul_boundary.md)

## 当前范围

默认只监听 `127.0.0.1`，一次连接、一条流、最多 10,000 条记录。录制文件有大小和条数上限；浏览目录有深度与候选数量上限。没有自动重连补发、任意区间录制、删除/重命名、跳转索引或按来源时间播放；这些可作为后续迭代。

FNV 校验用于发现意外内容变化，不是密码学认证；当前也不承诺断电持久性。项目没有生产部署或性能提升结论。

## 许可证

项目原创代码与文档采用 [MIT License](LICENSE)，Copyright © 2026 Zitao Zhang。

第三方代码遵循各自许可证：nlohmann/json 为 [MIT](third_party/nlohmann/LICENSE.MIT)，Boost 为 [Boost Software License 1.0](third_party/BOOST_LICENSE_1_0.txt)。第三方版权和许可声明保留，详见 [依赖说明](docs/dependencies.md)。
