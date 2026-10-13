# 备份恢复（T5.2）

> 对应验收项 A14：按预案完成一次数据库恢复演练，验证流程可执行、数据完整。
> 评审 P0-3 扩展（2026-10-12）：对象存储字节入预案——双资产叙事＋一致性核对
> 工具＋干净机器全量恢复演练。

## 资产边界

服务端持久数据是**双资产**，缺一不可：

1. **单库**（默认 `memex-server.db`，可用 `--db` 覆盖）：账号、设备台账、登录
   记录、消息归档、已读回执、撤回事件、离线补投、组织架构与通讯录可见性、
   群与成员角色、常用联系人、跨态日志、查阅审计、策略开关、webhook 台账、
   R23 文件**元数据**（files 秒传/引用计数/置顶、群／个人两级配额、外网
   uplink 预留日志）、表情包与头像 BLOB（落库不占对象卷）。
2. **对象卷**（RustFS/S3 兼容桶，如 `memex-files`）：R23 文件**正文字节**，
   内容寻址键 `groups/{gid}/{sha256}`、`users/{uid}/{sha256}`、
   `inbox/{uid}/…`。客户端永不直连对象存储，字节全过文件面。

元数据在库、字节在卷——**单库备份不含文件正文**；只备库＝文件面元数据
指向一个空桶。客户端直连态另有各机 `memex-local.db`（本机历史，不上报
服务端），不纳入服务端预案。

## 备份

### 单库

库文件在服务端**停写**（进程退出或不在线写入窗口）时可直接文件级复制：

```bash
memex_server serve --db /var/lib/memex/memex-server.db   # 正常运行时勿直接 cp
# 维护窗口：
systemctl stop memex-server   # 或 Ctrl-C
cp memex-server.db memex-server.db.$(date +%Y%m%d-%H%M%S).bak
```

在线热备建议用 SQLite 自带备份（`sqlite3 memex-server.db ".backup backup.db"`），
避免直接复制产生半写页。

### 对象卷（须与库**同窗口成对拍**）

对象卷与库必须**同一维护窗口内成对备份**：元数据行 ↔ 字节对象的一致性
以「同一时刻」为基准——只备卷不备库（或反之），恢复后就是半套。

```bash
# compose 部署（`storage compose` 生成的 rustfs-compose.yml，宿主目录挂载
# /var/lib/memex/rustfs；仓库根 docker-compose.yml 用命名卷 rustfs-data，
# tar 口径只适用于前者）：维护窗口内打 tar
tar czf rustfs-$(date +%Y%m%d-%H%M%S).tgz -C /var/lib/memex rustfs
# 或 S3 侧同步（不停机口径；恢复时须与某一库备份对拍核对）
mc mirror --overwrite local/memex-files /backup/memex-files   # 或 aws s3 sync
```

对象卷数据文件属 uid 10001（rustfs 容器运行身份），宿主删除/改属需
root 或经容器（演练脚本内有现成写法：`docker run --user 0 … chown`）。

## 恢复

```bash
systemctl stop memex-server
mv memex-server.db memex-server.db.bad   # 留痕，不直接覆盖
cp memex-server.db.<时刻>.bak memex-server.db
# 对象卷成对恢复（tar 口径）：
rm -rf /var/lib/memex/rustfs && mkdir -p /var/lib/memex/rustfs
tar xzf rustfs-<同时刻>.tgz -C /var/lib/memex
chown -R 10001:10001 /var/lib/memex/rustfs   # 部署口径：容器属主修复
systemctl start memex-server
memex_server account list --db memex-server.db   # 抽查
```

恢复后**必做一致性核对**（见下节）；文件面会话令牌只存内存，服务端重启
后客户端须重登（最小暴露面口径）。

## 一致性核对：`files verify`

恢复后核对元数据 ↔ 字节双面（只读工具，不代删不代改）：

```bash
memex_server files verify \
  --s3-endpoint http://127.0.0.1:9000 --s3-bucket memex-files \
  --s3-access-key AK --s3-secret-key SK        # 或 MEMEX_S3_* 环境变量
```

输出与退出码口径：

- **受检元数据行 / 桶内对象**：计数总览；
- **缺对象**（元数据在、字节无）与**大小不符**（元数据 file_size ≠ 对象
  实际大小）：**退出码 1**——恢复面有缺，禁止对外；
- **孤儿对象**（桶里有、无任何元数据行引用）：只告警——多为「落字节成功
  →元数据落库前崩溃」窗口残留，是否清理属运维决策；
- 隔离/过期行同样算引用面（其字节不算孤儿），但只核 normal 行的存在性
  与大小。

## 归档完整性账：`archive seal` / `archive verify`（评审四·远期加固）

消息归档（`message_events` 事件流）的防篡改账本——**不引入区块链**，分块
封账＋链式摘要：每 64 条事件在落库同事务自动封一块（`archive_chain` 表，
不可变封账行；未封开尾不落行），块内链式摘要
`h = sha256_hex(h + 事件摘要)`（首块 prev=64 个 `0`），单事件摘要用长度
前缀框架（`<len>:<bytes>`）防 payload 拼接歧义。

DB 级管理员可改库＝「append-only 表名≠不可篡改」——校验链断裂即可信性
断言：改／删／插任一已封事件行、或改账本行本身，逐块重放对不上账即如实
报断（`archive verify` 退出码 1，巡检脚本可挂）；未封开尾如实报告不判错
（正常态，待 seal 收尾）。

```bash
memex_server archive status   # 块数／开尾条数／链头摘要
memex_server archive seal     # 开尾收尾封账（定期运维动作，可挂日报）
memex_server archive verify   # 全账校验；完好 exit 0、已断 exit 1；
                              # 每次 verify 落 audit_reads 留痕可查
```

恢复场景的对账口径：库恢复后先 `archive verify`——事件流与账本逐位对上
才算恢复面完好；「校验不修复，只断言」（发现断链须从备份重放，不代修）。

## 演练（A14 可执行验证）

### 单库演练

仓库脚本 `scripts/backup_restore_drill.sh <memex_server 路径>`：

1. 临时目录造数（account add ×2）；
2. `account list` 记录备份前清单；
3. 文件级复制备份 → 删除原库 → 从备份恢复；
4. 恢复后 `account list` 与备份前逐行比对，不一致退出码非零。

2026-10-03 执行通过（本机 `build/server/memex_server`，退出码 0）。

### 干净机器全量恢复演练（双资产，评审 P0-3）

仓库脚本 `scripts/files_backup_drill.sh <memex_server 路径>`（依赖 docker
与 curl；端口可用 `API_PORT/CONSOLE_PORT/FILES_PORT/COLLAB_PORT` 覆盖）：

1. RustFS 容器起（`storage compose`＋`storage up`，宿主目录挂载；演练
   容器改名 `memex-rustfs-drill` 隔离——绝不碰宿主在跑的同名部署）；
2. 服务端起（单库＋文件面），造数：账号×2、个人上传×3；
3. **成对备份**：库 cp ＋ 对象卷 tar（同窗口拍）；
4. **清场＝干净机器**：停服务、库删除、桶归零（经 rustfs 镜像以属主身份
   清——数据文件属 uid 10001，宿主 rm 删不动）；
5. **成对恢复**：库回＋卷回＋`chown 10001:10001`（部署口径属主修复），
   RustFS 与服务端再起；
6. `files verify` 退出码 0（元数据 ↔ 字节一致）；
7. 权限关系抽查：会话随重启失效须重登；本人下载逐位一致、他人下载个人
   文件 403；
8. **核对工具自证**：真删一个对象字节再核——verify 必报不一致（退出码 1），
   核对器本身也在演练范围内。

2026-10-12 执行通过（本机 `build-ci/server/memex_server`，退出码 0），
实跑输出：

```
[drill] RustFS 就绪 http://127.0.0.1:19290（数据目录 /tmp/tmp.w5uKjgY0Zx/data）
[drill] 服务端就绪（files :18443，库 /tmp/tmp.w5uKjgY0Zx/memex-server.db）
[drill] 造数完成：alice×2 ＋ bob×1（id 1/2/3）
[drill] 备份完成：库 626688 字节＋对象卷 7522 字节
[drill] 清场完成（库与桶均归零）
[drill] 恢复完成，服务端再起
受检元数据行（normal）：3　桶内对象：3
一致：元数据与对象字节核对通过（孤儿 0 条）
[drill] files verify：一致
[drill] 权限关系：alice 本人可下（字节一致）、bob 下载 alice 文件 403、bob 本人可下
[drill] 核对工具自证：缺对象必报不一致
[drill] 演练通过：干净机器全量恢复 → 一致性核对 → 权限关系正确
```

每次服务端大版本升级后应重复两场演练。
