# 排错案例：原始 NUL 隐藏 JSON 尾部

日期：2026-10-15。性质：本项目命令行检查程序开发期间的真实测试发现，不是生产事故，也不是故意混入正常库的错误。该问题在 Codex 协作开发与测试中定位和修复。

## 复现条件与失败证据

环境：macOS arm64、Apple Clang 21.0.0、nlohmann/json 3.11.3、Debug/Release。合法 numeric JSON 后追加原始字节 `0x00`、`junk` 和换行，写到受控本地文件。期望整个输入被拒绝。

命令：`./build/cmake-debug/streamdesk_inspect build/verification/nul-reproduction.jsonl`。

首次实际结果（修复前）：

```text
Expected exit: nonzero
Actual exit: 0
INSPECT {"count":1,"sum":1}
```

原文件 160 字节，结果却只反映 NUL 前的合法 JSON。证据：`build/verification/r5-nul-before-fix.log`。Debug 与 Release 集成测试均失败，CTest 为 4/5、退出 8，日志 `build/verification/r4-initial-{debug,release}-test.log`；Python 失败点为 `inspector accepted corrupt/overlong input`。

## 缩小问题与根因

先确认不是 TCP 随机分块：直接读取同一个本地损坏文件即可稳定复现。检查 `examples/inspect_numeric.cpp`，读取使用固定数组，但调用 `decode_numeric(line.data())` 隐式从 C 字符串构造 string_view，长度在第一个 NUL 截断。

进一步给核心测试直接传入 `valid + std::string("\0junk", 5)`，显式保留 NUL 与后缀长度，仍出现：

```text
FAIL: raw NUL cannot hide trailing data
Exit status: 1
```

证据：`build/verification/r5-nul-core-before-fix.log`。这排除了仅修复调用方长度就足够的假设。读取固定上游头文件的 lexer 可见 `case '\0'` 与 EOF 一同返回 end_of_input（当前固定文件约第 8958 行）；本协议是长度定界的 JSON 正文，不能把原始 NUL 当合法终止符。

这是输入长度与解析终止语义不一致造成的校验缺口，不是“ASan 没报错”能证明正确的问题。本案例使用测试输出、输入字节和源码定位证据，没有声称使用过未执行的调用栈或 Sanitizer。

## 修复与回归

1. 检查程序使用 getline 的实际 gcount，并扣除已消费但未存储的换行；构造显式长度 string_view，不用隐式 strlen。
2. `decode_numeric` 在调用 JSON 库之前拒绝正文中的原始 NUL。转义形式 `\u0000` 不会在此预检被误判为原始字节，解码后仍由文本字段的控制字符规则拒绝。
3. 保留原有 JSON 库和协议，不重写解析器、不删除测试。
4. 核心测试加入原始 NUL+尾部检查；集成测试同时覆盖文件入口和带正确长度头的 TCP 正文入口。

修复后同一文件真实输出：

```text
ERROR: raw NUL byte in JSON body
Exit status: 1
```

证据：`build/verification/r5-nul-after-fix.log`。两个构建的完整 CTest 均恢复为 5/5、退出 0，日志 `build/verification/r5-nul-fixed-{debug,release}-test.log`；numeric 测试包含 81 项检查。正常 5 条/1000 条字段、顺序和统计核对继续通过，未通过放宽验证获得绿色结果。

## 局限与讲解重点

本案例证明上述固定输入的校验缺陷被发现并有回归，不代表所有恶意 JSON、所有平台或内存线程问题均已证明安全。Ubuntu 尚未运行。关键在于：为什么 length-delimited 输入不能退化成 C 字符串，以及为什么第一次定位后还需要进一步隔离核心解析行为。

复现文件与原始日志在忽略的 build 目录，当前本机保留；本说明保存关键输出与过程，长期可复现的修复后用例保留在 numeric_test.cpp 与 tcp_test.py。无需恢复错误源码来运行回归。
