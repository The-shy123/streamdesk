# 项目内依赖

不安装全局依赖，不追踪最新分支。核心公共头文件不暴露第三方类型。

## nlohmann/json 3.11.3

用途：numeric v1 和录制容器 v1 的 JSON 语法解析与序列化。MIT，许可证位于 `third_party/nlohmann/LICENSE.MIT`。

官方来源：[3.11.3 发布](https://github.com/nlohmann/json/releases/tag/v3.11.3)，单头文件下载 `https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp`；许可证通过官方 GitHub API 的 `LICENSE.MIT?ref=v3.11.3` 取得。raw.githubusercontent.com 本机连接重置，改用同一官方版本的发布附件与 API，未更换来源或关闭 TLS 校验。

头文件保存在 `third_party/nlohmann/include/nlohmann/json.hpp`，不修改上游内容，仅作为 StreamCore 私有 include 使用。完整源码拷贝含此文件后，核心构建无需联网。

SHA-256：
- json.hpp：`9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`
- LICENSE.MIT：`86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14`

实际兼容性以当前测试记录为准；不声称验证了库支持的全部平台。

## Boost 1.88.0 / Boost.Asio

用途：仅服务端的 TCP 异步接入、计时器与停止信号；核心库不依赖 Boost。使用头文件方式，不编译/安装全局库。Boost Software License 1.0，副本 `third_party/BOOST_LICENSE_1_0.txt`。

官方固定包：`https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.bz2`。官方同名 `.json` 校验元数据给出 SHA-256：`46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b`。本机下载后已校验，仅提取 boost/ 头文件和许可证到 `build/deps/boost_1_88_0`，约 137 MiB 下载，不修改系统。

可复现步骤：`python3 tools/fetch_boost.py`。已有下载包先验哈希再使用；不匹配时停止，不静默覆盖。仅启用 `STREAMDESK_BUILD_SERVER=ON` 才需要此依赖；默认核心构建不需要联网或 Boost。`STREAMDESK_BOOST_ROOT` 可指向同版本项目内缓存，CMake 核对版本。

[官方 Boost.Asio 使用说明](https://www.boost.org/doc/libs/1_88_0/doc/html/boost_asio/using.html)。当前版本不依赖 Qt 或 Drogon。

## 项目许可证

项目原创代码和文档使用根目录 [MIT License](../LICENSE)。上述第三方代码与随附许可仍适用各自声明，项目 MIT 不替代第三方版权和许可。
