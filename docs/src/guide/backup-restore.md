# Backup and Restore (T5.2)

> Corresponds to acceptance item A14: run one database recovery drill per the plan, verifying the procedure is executable and the data is intact.
> Review P0-3 extension (2026-10-12): object storage bytes enter the plan — dual-asset narrative + consistency check tool + full recovery drill on a clean machine.

## Asset Boundary

The server's persistent data is a **dual asset**, and neither half is dispensable:

1. **Single database** (default `memex-server.db`, overridable with `--db`): accounts, device registry, login records, message archive, read receipts, recall events, offline redelivery, org structure and address-book visibility, groups and member roles, frequent contacts, cross-mode logs, access audits, policy switches, webhook registry, R23 file **metadata** (files dedup-by-hash / reference counting / pinning, group/personal two-tier quotas, external uplink reservation logs), and sticker/avatar BLOBs (persisted in the database, not occupying the object volume).
2. **Object volume** (RustFS/S3-compatible bucket, e.g. `memex-files`): R23 file **content bytes**, content-addressed keys `groups/{gid}/{sha256}`, `users/{uid}/{sha256}`, `inbox/{uid}/…`. The client never connects to object storage directly; all bytes go through the file plane.

Metadata in the database, bytes in the volume — **a database-only backup contains no file content**; backing up only the database = file-plane metadata pointing at an empty bucket. The client's direct mode additionally has a per-machine `memex-local.db` (local history, never reported to the server), which is outside the server's plan.

## Backup

### Single Database

The database file can be copied at the file level while the server is **not writing** (process exited or within an offline write window):

```bash
memex_server serve --db /var/lib/memex/memex-server.db   # 正常运行时勿直接 cp
# 维护窗口：
systemctl stop memex-server   # 或 Ctrl-C
cp memex-server.db memex-server.db.$(date +%Y%m%d-%H%M%S).bak
```

For online hot backups, SQLite's own backup is recommended (`sqlite3 memex-server.db ".backup backup.db"`), to avoid half-written pages from direct copying.

### Object Volume (Must Be Backed Up **Paired in the Same Window** as the Database)

The object volume and the database must be **backed up as a pair within the same maintenance window**: the consistency between metadata rows and byte objects is anchored to "the same moment" — backing up only the volume and not the database (or vice versa) leaves you with half a set after recovery.

```bash
# compose 部署（`storage compose` 生成的 rustfs-compose.yml，宿主目录挂载
# /var/lib/memex/rustfs；仓库根 docker-compose.yml 用命名卷 rustfs-data，
# tar 口径只适用于前者）：维护窗口内打 tar
tar czf rustfs-$(date +%Y%m%d-%H%M%S).tgz -C /var/lib/memex rustfs
# 或 S3 侧同步（不停机口径；恢复时须与某一库备份对拍核对）
mc mirror --overwrite local/memex-files /backup/memex-files   # 或 aws s3 sync
```

The object volume's data files belong to uid 10001 (the rustfs container's runtime identity); host-side deletion or ownership changes require root or going through a container (the drill script has a ready-made pattern: `docker run --user 0 … chown`).

## Recovery

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

After recovery a **consistency check is mandatory** (see the next section); file-plane session tokens live only in memory, so clients must re-login after a server restart (minimal exposure surface convention).

## Consistency Check: `files verify`

After recovery, check the metadata ↔ bytes dual side (read-only tool; deletes and modifies nothing on your behalf):

```bash
memex_server files verify \
  --s3-endpoint http://127.0.0.1:9000 --s3-bucket memex-files \
  --s3-access-key AK --s3-secret-key SK        # 或 MEMEX_S3_* 环境变量
```

Output and exit-code conventions:

- **Checked metadata rows / objects in the bucket**: count overview;
- **Missing objects** (metadata present, bytes absent) and **size mismatch** (metadata file_size ≠ the object's actual size): **exit code 1** — the recovery surface has gaps; do not serve traffic;
- **Orphan objects** (present in the bucket, referenced by no metadata row): warning only — mostly residue from the "bytes persisted → crash before metadata commit" window; whether to clean up is an operations decision;
- Quarantined/expired rows also count toward the reference side (their bytes are not orphans), but only normal rows are checked for existence and size.

## Archive Integrity Ledger: `archive seal` / `archive verify` (Review Four · Long-Term Hardening)

A tamper-evident ledger for the message archive (the `message_events` event stream) — **no blockchain introduced**; block sealing + chained digests: every 64 events are automatically sealed into one block in the same transaction as the commit (`archive_chain` table, immutable seal rows; an unsealed tail writes no rows), with an in-block chained digest `h = sha256_hex(h + event digest)` (the first block's prev = 64 `0`s); single-event digests use a length-prefixed framing (`<len>:<bytes>`) to prevent payload-concatenation ambiguity.

A DB-level administrator can modify the database = "an append-only table name ≠ tamper-proof" — a broken verification chain is itself the trust assertion: modifying / deleting / inserting any sealed event row, or modifying the ledger rows themselves, makes the per-block replay fail to match, and the break is reported as-is (`archive verify` exit code 1; inspection scripts can hook it); an unsealed tail is reported truthfully and not judged an error (normal state, awaiting seal wrap-up).

```bash
memex_server archive status   # 块数／开尾条数／链头摘要
memex_server archive seal     # 开尾收尾封账（定期运维动作，可挂日报）
memex_server archive verify   # 全账校验；完好 exit 0、已断 exit 1；
                              # 每次 verify 落 audit_reads 留痕可查
```

Reconciliation convention for recovery scenarios: after the database is restored, run `archive verify` first — the event stream and the ledger must match bit for bit for the recovery surface to be intact; "verify does not repair, it only asserts" (a discovered break must be replayed from a backup; the tool does not repair on your behalf).

## Drills (A14 Executable Verification)

### Single-Database Drill

Repo script `scripts/backup_restore_drill.sh <memex_server path>`:

1. Create data in a temp directory (account add ×2);
2. `account list` records the pre-backup inventory;
3. File-level copy backup → delete the original database → restore from the backup;
4. After recovery, `account list` is compared line by line with the pre-backup inventory; any mismatch yields a non-zero exit code.

Passed on 2026-10-03 (local `build/server/memex_server`, exit code 0).

### Clean-Machine Full Recovery Drill (Dual Asset, Review P0-3)

Repo script `scripts/files_backup_drill.sh <memex_server path>` (depends on docker and curl; ports can be overridden with `API_PORT/CONSOLE_PORT/FILES_PORT/COLLAB_PORT`):

1. Start the RustFS container (`storage compose` + `storage up`, host-directory mount; the drill container is renamed `memex-rustfs-drill` for isolation — never touch the same-named deployment running on the host);
2. Start the server (single database + file plane); create data: accounts ×2, personal uploads ×3;
3. **Paired backup**: database cp + object volume tar (same-window pair);
4. **Clean slate = clean machine**: stop the service, delete the database, zero the bucket (cleared via the rustfs image as the owner identity — the data files belong to uid 10001, which host rm cannot delete);
5. **Paired restore**: database back + volume back + `chown 10001:10001` (deployment-convention ownership repair), then restart RustFS and the server;
6. `files verify` exit code 0 (metadata ↔ bytes consistent);
7. Permission spot checks: sessions are invalidated by the restart and require re-login; the owner's download matches bit for bit, another user downloading a personal file gets 403;
8. **The check tool proves itself**: actually delete one object's bytes and re-check — verify must report inconsistency (exit code 1); the checker itself is within the drill's scope.

Passed on 2026-10-12 (local `build-ci/server/memex_server`, exit code 0); actual run output:

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

Both drills should be repeated after every major server upgrade.
