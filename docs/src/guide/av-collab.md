# 音视频会议与协同文档底座

音视频（LiveKit）与协同文档（OnlyOffice）以**独立进程**部署，
与 memex 协作服务端保持进程边界：媒体字节与文档字节不进 memex 归档库；
会议元数据（房间／成员／起止）与文档会话元数据经回调接口留痕
（webhook → 归档，接口预留，客户端入口后续）。

## 部署

```bash
# 音视频：LiveKit（内嵌 TURN，对称 NAT 可达）
docker compose -f scripts/deploy/meet-compose.yml up -d
# 协同文档：OnlyOffice DocumentServer
docker compose -f scripts/deploy/docs-compose.yml up -d
```

| 服务 | 地址 | 说明 |
|---|---|---|
| LiveKit HTTP API | `http://宿主机:7880/` | 建房／列房／删房（API key 鉴权） |
| LiveKit RTC/TCP | `宿主机:7881` | UDP 不通时的媒体回落 |
| LiveKit TURN | `宿主机:3478`（UDP/TCP；TLS 需证书，生产反代后启用） | 对称 NAT 穿透，内嵌实现 |
| OnlyOffice | `http://宿主机:8800/` | 编辑器、API、`/healthcheck` |

## 验证

```bash
bash scripts/deploy/av_docs_drill.sh
```

演练内容：compose 配置校验 → 拉起 → 建房／列房／删房 API 全通 →
TURN Allocate 成功（穿透凭据链）→ OnlyOffice 健康检查 → 自动回收。
通过标准：脚本 exit 0（6 步全部绿）。

## 选型

- 会议：LiveKit（Apache-2.0，自部署 SFU，内嵌 TURN 少一个组件）。
  备份：Jitsi（Apache-2.0，jvb＋jicofo＋meet 三件套，运维面更重）。
- 文档：OnlyOffice（AGPL-3.0，见下）。
  备份：Collabora（MPL-2.0，无网络条款）。

## 许可与合规

OnlyOffice 为 AGPL-3.0：使用官方镜像**原样运行**（不修改、不二次分发），
memex 二进制不链接其任何代码（进程边界即许可边界）。交付时提供
`third_party/README.md` 登记表＋镜像 digest（`docker inspect`），
上游源码：https://github.com/ONLYOFFICE/DocumentServer。
若 AGPL 流程不可接受，切换 Collabora 备选。

## 上线前 hardening 清单

- [ ] 替换全部演示凭据（`livekit.yaml` keys、OnlyOffice `JWT_SECRET`、
      TURN 静态账号），密钥走内网 KMS／保险柜下发。
- [ ] LiveKit／OnlyOffice 置于 TLS 反代后（证书内网 CA 签发），
      OnlyOffice 关 `ALLOW_PRIVATE_IP_ADDRESS`。
- [ ] 会议 webhook（房间起止／成员进出）接入 memex 归档（元数据归档，
      音视频流本身按留痕口径以纪要形式归档）。
- [ ] 文档回调（保存／版本）接入归档：谁何时编了哪篇（无正文即元数据，
      正文版本由 DocumentServer 自管）。
- [ ] 客户端入口：会话内「发起会议」「协同编辑」按钮（xdg-open 房间 URL／
      文档 URL），保持进程边界（不嵌 SDK）。
