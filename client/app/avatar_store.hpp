// 用户头像工具面（需求批⑫）：内置默认头像组（按账号 hash 稳定取，同账号
// 跨端一致）＋已设头像的本地缩放缓存（AppDataLocation，key=账号+版本戳+档位，
// 防重复缩放；MEMEX_TEST_AVATAR_DIR 覆盖供测试落盘到临时目录）。
// 服务端字节只取 256 档，128/64/32 由本面缩放派生（见 main_window 接线）。
#pragma once

#include <QPixmap>
#include <QString>

namespace memex::client {

// 内置默认头像张数（:/avatars/avatar-<1..N>.png）
constexpr int kDefaultAvatarCount = 4;

// 账号 → 默认头像下标（1 起）：FNV-1a 逐字节（UTF-8）后取模——qHash
// 跨 Qt 版本不保证稳定，手写保跨端一致
int default_avatar_index(const QString& account);

// 默认头像（按账号 hash 稳定取；平滑缩放到 size 见方）
QPixmap default_avatar(const QString& account, int size);

// 缩放缓存目录：MEMEX_TEST_AVATAR_DIR 优先（测试隔离），否则
// AppDataLocation/avatars（目录不存在时缓存写入方负责创建）
QString avatar_cache_dir();

// 缓存命中读取（key=账号+版本戳+档位；未命中返回空 QPixmap）
QPixmap cached_avatar(const QString& account, qint64 ver, int size);

// 缩放结果落缓存（磁盘失败静默——缓存缺失只影响下次缩放耗时）
void store_avatar_cache(const QString& account, qint64 ver, int size,
                        const QPixmap& pixmap);

// 展示面统一入口：ver>0 先查缓存（未命中回落默认头像——下载到位后由
// 调用方补缓存再刷新），ver=0 恒默认头像
QPixmap avatar_for(const QString& account, qint64 ver, int size);

// tooltip 富文本 <img src>：已设头像给缓存文件路径（未缓存回落默认头像
// 资源路径），供会话列表悬浮 tooltip 拼 HTML
QString avatar_tooltip_src(const QString& account, qint64 ver, int size);

} // namespace memex::client
