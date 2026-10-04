# 备份恢复（T5.2）

> 对应验收项 A14：按预案完成一次数据库恢复演练，验证流程可执行、数据完整。

## 资产边界

协作服务端的全部数据在单个 SQLite 库（默认 `memex-server.db`，可用 `--db` 覆盖）：
账号、设备台账、登录记录、消息归档、已读回执、撤回事件、离线补投、组织架构与
通讯录可见性、群与成员角色、常用联系人、跨态日志、查阅审计、策略开关、webhook 台账、
R23 文件元数据（files 秒传/引用计数/置顶、群／个人两级配额、外网 uplink 预留日志）。
客户端直连态另有各机 `memex-local.db`（本机历史，不上报服务端），不纳入服务端预案。

## 备份

库文件在服务端**停写**（进程退出或不在线写入窗口）时可直接文件级复制：

```bash
memex_server serve --db /var/lib/memex/memex-server.db   # 正常运行时勿直接 cp
# 维护窗口：
systemctl stop memex-server   # 或 Ctrl-C
cp memex-server.db memex-server.db.$(date +%Y%m%d-%H%M%S).bak
```

在线热备建议用 SQLite 自带备份（`sqlite3 memex-server.db ".backup backup.db"`），
避免直接复制产生半写页。

## 恢复

```bash
systemctl stop memex-server
mv memex-server.db memex-server.db.bad   # 留痕，不直接覆盖
cp memex-server.db.<时刻>.bak memex-server.db
systemctl start memex-server
memex_server account list --db memex-server.db   # 抽查
```

## 演练（A14 可执行验证）

仓库脚本 `scripts/backup_restore_drill.sh <memex_server 路径>`：

1. 临时目录造数（account add ×2）；
2. `account list` 记录备份前清单；
3. 文件级复制备份 → 删除原库 → 从备份恢复；
4. 恢复后 `account list` 与备份前逐行比对，不一致退出码非零。

演练输出示例：

```
备份前：
账号	展示名（角色）
alice	爱丽丝（admin）
bob	鲍勃（member）
恢复后：（同上）
演练通过：备份 → 恢复后数据完整
```

2026-10-03 执行通过（本机 `build/server/memex_server`，退出码 0）。
每次服务端大版本升级后应重复本演练。
