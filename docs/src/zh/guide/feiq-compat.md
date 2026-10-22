# 飞秋/IPMSG 兼容

> 需求（2026-10-11 用户令）：同网段有飞秋客户端时，memex 出现在其在线表并可单聊互通。
> 口径：飞秋兼容是**旁路**（默认关，设置页显式开启），不改变 memex 直连/协作两态的
> 任何行为；文件传输、加密、广播群发等 IPMSG 高级特性不实现。

## 开关与入口

「网络与远程」设置页新增「飞秋兼容」开关（`net/ipmsg_enabled`，默认关）。开启即时生效：
同口 UDP 2425 立即补发一条 IPMSG `BR_ENTRY` 广播，飞秋在线表即互见；关闭后 IPMSG 报文
静默丢弃，socket 只走 memex JSON 发现。

## 协议口径

同一 UDP 2425 口按**首字节分流**：`{`＝memex JSON 宣告（原发现通道，行为不变）；数字
开头＝IPMSG 报文（`ver:packet_no:user:host:command:additional`，命令区十六进制）。

| 面 | 行为 |
|---|---|
| 在线表 | 周期伴发 `BR_ENTRY`（与 JSON 宣告同拍）；收到飞秋 `BR_ENTRY` 必回 `ANSENTRY`（协议握手——不回对方在线表就没有我）；`BR_EXIT` 收到即摘行 |
| 身份 | 本端头部 user＝协作账号（未登录退化设备名），host 恒 `memex`——收到 host=memex 的 IPMSG 包即另一 memex 实例（已由 JSON 通道登记），IPMSG 侧不建行，防同机互见重复行 |
| 单聊收 | `SENDMSG` 必回 `RECVMSG`（原包号回填附加区，对端送达勾点靠这条）；消息体按 `UTF8OPT` 位解码（置位＝UTF-8，否则 GB18030——GBK 的超集，真飞秋字节解出无损） |
| 单聊发 | 先 GBK/CP936（真飞秋 Windows ANSI 码页口径）；emoji 等 GBK 外字符编不出回退 UTF-8 并置 `UTF8OPT`（不退 GB18030——超集编得出的字节老飞秋照样乱码，不如让带位的新版对端按位解） |
| 回执 | 发送带 `SEND_CHECKOPT`；对端 `RECVMSG` 按同号回填本地 `text_delivered`——包号即调用方发送序号，界面送达态直接对齐 |
| 未知命令 | 静默忽略（新版特性包不回不崩） |

编解码为纯函数面（`client/engine/direct/ipmsg.{hpp,cpp}`），收发与字符集都在这里，
网络侧（discovery）只做分流与路由；不入列选项、密文（`SECRETOPT` 位仅定义、无消费点，
收到即忽略——消息体一律按 `UTF8OPT` 位解码）、文件附件等高级行为不实现。

## 界面与落库

飞秋对端以 `ipmsg:用户@主机` 形态进直连在线表（元信息行标「飞秋 · UDP」），单聊收发与
memex 直连消息同径：本地 SQLite 落库（`source=direct`，不进服务端归档）、未读角标、系统
通知、跨态闭环全部复用。

## 验收证据（2026-10-12 本机）

- `test_ipmsg`（ctest 名 `ipmsg`）：编解码往返与畸形拒（段数/进制/JSON）；mock 飞秋对拨
  （GB18030/GBK 中文双向、RECVMSG 原包号回执、emoji→UTF8OPT 兜底、`BR_EXIT` 摘行、关断
  静默、memex 标记不重复建行）；直连引擎全链（`send_text` 分流→回执关单、收文落库上抛）。
- `ss -ulpn`：客户端开启兼容后监听 UDP 2425（飞秋/IPMSG 固定端口），见下节快照。

## 已知边界（诚实注记）

- **真飞秋客户端实测未做，缺环境**——本机（Linux）无飞秋客户端可装，interop 证据止于
  协议口径对拍（常量与报文格式按飞秋源码 ipmsg.h 与 compilelife/feiq feiqlib 对拍定案）
  与 mock 飞秋对拨。真机跑通前，请把「能与真飞秋互见互发」当**未验证**处理。
- 同机 memex↔飞秋对拨（都绑 2425）时，SO_REUSEADDR 下单播只进最后绑定 socket——真实
  场景（飞秋在别机）不受影响；测试 mock 按绑定次序契约规避（见 `test_ipmsg.cpp` 头注）。
- 飞秋的文件传输、加密、离开模式自动应答未实现，收到即忽略对应选项位。
