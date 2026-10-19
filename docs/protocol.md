# numeric v1 协议

本协议细化 PROJECT 第 8 节，第一版仅一源一流，不改变技术路线。R1 使用 JSON 正文；R2 增加 4 字节无符号网络字节序长度头，长度不含头。正文长度 1..65536 字节，在解析/分配正文前检查；UTF-8 JSON 对象，禁止重复键、未知字段、注释、原始 NUL 字节与尾随文本。解析嵌套深度最多 8，避免深递归。JSON 交给固定 nlohmann/json，不手写 JSON 解析器。

| 字段 | 类型与规则 |
| --- | --- |
| schema_version | 必填 JSON 整数，严格为 1，不接受 1.0、布尔值或未知版本 |
| source_id / stream_id / session_id | 必填字符串，各 1..64 字节，仅 ASCII 字母数字及 `_-.` |
| seq | 必填十进制字符串：`0` 或无前导零的数字，范围 0..18446744073709551615；不得使用 JSON 数字或浮点中转 |
| kind | 必填字符串，严格为 numeric；image/market 尚不支持 |
| payload | 必填对象，仅 name/value/unit |
| payload.name | 必填 UTF-8 字符串，1..128 字节，不含 ASCII 控制字符 |
| payload.value | 必填 JSON number，转换为 double 后有限；布尔、字符串、null、NaN、Infinity、溢出拒绝 |
| payload.unit | 可选，出现时为非空 UTF-8 字符串，最多 16 字节，无 ASCII 控制字符；null 不等于缺省 |
| source_time | 可选对象；必填 value 与 unit，value 为 uint64 十进制字符串，unit 严格为 us；只表示源端微秒数，不承诺时钟同步 |

合法最小样例：

```json
{"schema_version":1,"source_id":"sim","stream_id":"values","session_id":"run1","seq":"0","kind":"numeric","payload":{"name":"temperature","value":21.5}}
```

C++ NumericMessage 拥有自身的字符串和可选字段，输入 JSON 缓冲销毁后仍有效。`decode_numeric` 返回已校验模型；`validate_numeric` 接受直接构造的模型；`encode_numeric` 必须先校验，不能把 NaN 静默写成 null。语法、字段和范围错误抛出 `ProtocolError`；分配失败保持原异常，不误报普通非法输入。

往返按字段语义比较，不比较空格、字段顺序或小数的文本格式。value 是 double 测量值，不保证任意精度整数语义；需要精确计数的 seq 与 source_time.value 始终采用 uint64 和字符串。seq 是否递增、重复、缺口属于连接会话检查，不属于单条 JSON 语法。

当前不支持数组正文、多流、二进制图像、重发、录制格式或精确一次保证。

## 增量长度解析（R2）

`FrameParser::push` 返回本次消耗字节数和最多一条拥有自身内存的正文。调用者保留未消耗尾部并继续输入；解析器不保存无限输出列表。内部最多保留 4 字节头和 65536 字节正文；长度为 0、65537 或 0xffffffff 都在分配正文前拒绝。空输入可以消耗 0 字节，非空输入的正常推进必须消耗字节。

`finish` 表示 EOF：完整边界成功，残留头/正文失败；成功后不能继续 push，重复 finish 安全。协议错误后进入失败状态，必须显式 reset 才能处理新连接。reset 清空原连接状态，不能用它隐藏同一连接的坏数据。正文 JSON 合法性仍由 R1 检查。
