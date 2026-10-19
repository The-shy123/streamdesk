# 数值数据保存与回放 v1

更新：2026-10-18。当前实现为单连接、单流、固定条数录制和本地命令行回放，处理逻辑在 C++ StreamCore 中。已有 `--output` JSONL 仍是核对输出；正式录制使用 `--record` 生成 `.sdr` 文件，两者不能互相冒充。

## 记录浏览入口（2026-10-19）

已有记录可通过 `./build/cmake-debug/streamdesk_records build --interactive` 按编号查看和选择回放。默认扫描指定目录及下一层子目录；停止或完成回放后返回列表。详情见 [records.md](records.md)。下方独立回放与手动录制命令继续可用。

## 使用

按 README 构建后，一键完成接入、保存和回放核对：

```sh
python3 tools/demo.py --record
```

终端会显示实际 `Recording: /.../numeric.sdr` 路径。每次创建独立目录，保存原始数据、服务日志、检查日志、回放日志和 `verification.json`。不复用或覆盖上次结果。未加 `--record` 时保留原先的数据通路演示。

手动采集时，在项目根目录的终端 A 执行：

```sh
capture_dir=$(mktemp -d "$PWD/build/capture.XXXXXX")
./build/cmake-debug/apps/server/streamdesk_server --port 19090 --record "$capture_dir/sample.sdr" --record-count 1000 --deadline-seconds 120
```

看到 READY 后，终端 B 发送：

```sh
python3 tools/numeric_source.py --port 19090 --count 1000 --fragment 17
```

成功时终端 A 出现 `RECORDING completed .../sample.sdr`。仍在该终端中，可以完整顺序回放：

```sh
./build/cmake-debug/streamdesk_replay "$capture_dir/sample.sdr"
```

或进入交互控制，先暂停等待命令：

```sh
./build/cmake-debug/streamdesk_replay "$capture_dir/sample.sdr" --interactive --interval-ms 100
```

| 输入并回车 | 行为 |
| --- | --- |
| `next` | 读出下一条，然后保持暂停 |
| `play` / `resume` | 从当前位置连续播放 |
| `pause` | 确认暂停后不再读取后续记录 |
| `status` | 显示状态和已播放条数/总条数 |
| `stop` / `quit` | 停止并退出，状态为 stopped，保留实际进度 |
| Ctrl+C；交互输入 EOF | 可中止播放或暂停状态，退出为 stopped |

每条输出为 `VALUE <numeric JSON>`，最后输出 `REPLAY` 摘要。默认播放无人工间隔；`--interval-ms` 为固定间隔 0..1000 毫秒，不模拟来源时间戳。独立回放命令换文件时重新启动；记录浏览器可以 stop 返回列表后选择另一条。没有跳转或图形页面。

## 录制范围与状态

`--record PATH` 必须同时指定 `--record-count N`，N 为 1..10000，且不大于 `--max-records`。这是本版明确采集区间的方法：服务在 READY 前独占保留 `PATH.partial`；首条合法模型确定流、会话与起始 seq，随后只能接收同一会话连续的 N 条。

所有记录按现有工作线程的顺序追加。网络事件循环不执行文件写入或排空等待。正常 EOF 后，控制线程先等待已接受任务处理结束，再收尾文件。发布条件同时包括：正常消息边界的 EOF、正好 N 条、无协议/处理/文件错误、成功写入结束信息和关闭文件。多一条、少一条、半包、序号不连续、任务错误均不能完成。

`Creating → Recording → Finalizing → Completed` 对应创建临时文件、追加、校验收尾、正式路径出现。发生错误则运行结果为 failed；主动停止为 interrupted。进程被强制终止时不会执行收尾，留下 `.partial`，下次打开它明确报告 interrupted/unpublished。当前没有历史数据库；记录浏览器只在显式指定目录内做有界发现，不做全盘扫描；同路径重新录制会拒绝已有临时文件，原数据保留。

Ctrl+C 不被当作“完整保存已收到前缀”。本版只有预先声明的 N 条区间能完成；任意时刻开始/停止并保存一个可变区间的控制协议属于后续功能。

## 文件格式

扩展名约定为 `.sdr`，UTF-8、LF 换行。每行必须有结尾 LF，单行最多 65536 字节正文，整文件最多 16 MiB。所有行使用本项目生成的规范化紧凑 JSON；未知字段、重复键或非规范容器形式会被拒绝。

1. 文件头：`format=streamdesk.numeric`、`version=1`、`expected_count`、`source_id`、`stream_id`、`session_id`、字符串 `first_seq`。
2. N 条正文：直接使用现有 numeric v1 的规范化编码，保留全部领域字段与可选字段，不写 C++ 内存布局。
3. 文件尾：`recording_end=completed`、整数 `count`、字符串 `last_seq`、16 位小写十六进制 `checksum_fnv1a64`。

FNV-1a 64 位校验从 14695981039346656037 开始，对文件头和每条记录的实际 UTF-8 字节（包括 LF）依次异或并乘 1099511628211，按 uint64 模 2^64 溢出。文件尾不纳入校验。它用于发现意外内容变化，与数量、流和序号校验组合使用；不是密码学签名或防篡改认证。

录制器用排他创建保留 `.partial`，不会跟随该路径已有的符号链接；收尾后用不替换已有目标的硬链接发布正式路径，再移除临时别名。若目标已存在或发布时出现同名文件，操作失败并保留原文件。若成功发布后临时别名清理失败，正式文件仍有效，多余 `.partial` 仍按未发布文件拒绝回放；不自动删除不明文件。

追加检查写入与刷新结果，收尾检查关闭结果；没有承诺断电持久性，也未调用 fsync。当前排他文件创建与回放输入控制采用 POSIX 接口，实际验证平台为 macOS。

## 回放与资源边界

`RecordingReader` 构造时完整预检文件：大小、版本、每条模型、连续性、结束信息、校验值和尾部 EOF。损坏文件在任何 VALUE 输出前失败；旧核对 JSONL 不能被当作正式录制打开。

预检与播放都按行流式读取，正文缓冲有上限。预检结束后复用同一个文件流回到开头，`next()` 每次仅交付一条拥有数据的模型，并再次检查领域规则和结束信息；不把全部记录留在内存中。回放期间不要修改原文件。

命令行只有一个控制循环，串行管理读取与状态。暂停不启动后台预取队列；停止后不继续输出下一条。交互命令长度上限 128 字节，等待轮询有界，Ctrl+C 可以打断暂停或播放。预检本身最多读取 16 MiB，完成预检后才报告 REPLAY_READY。

## 验证入口

- [核心测试](../libs/streamcore/tests/recording_test.cpp)：逐字段往返、大整数/中文、元数据、文件损坏、大小限制、未完成文件、失败状态、同路径冲突和符号链接保护。
- [集成测试](../tests/integration/recording_test.py)：真实 TCP 采集 1/10000 条，原始源、核对输出、正式文件和回放逐字段及顺序比较；Python 独立计算校验值。另测提前 EOF、半包、超额、停止、进程中断、操作系统写入失败、坏文件预检及交互控制。
- 写入失败通过仅限制测试子进程的文件大小触发，不写满磁盘、不修改系统全局限额。
- 最新构建、执行结果与复现命令见 [验证记录](validation.md)。

本版完成保存和本地顺序回放这个功能子集；本地目录列表、详情和选择回放已于 2026-10-19 实现；HTTP 下载、持久化历史索引、GUI、任意区间控制、跳转索引和按来源时间播放仍未实现。
