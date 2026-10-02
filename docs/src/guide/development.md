# 克隆与构建

## 环境要求

- CMake 4.x、C++20 编译器（GCC 15 或 MSVC）
- asio（Linux：`libasio-dev`）
- 客户端另需 Qt 6（含 Qt SQL SQLite 驱动）
- 文档站：Node 22 + pnpm

## 构建与测试

```bash
git clone https://github.com/cuihairu/memex.git
cd memex

# 服务端与公共库
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# 客户端（需 Qt 6）
cmake -B build -S . -DMEMEX_BUILD_CLIENT=ON
cmake --build build
```

## 文档站

```bash
cd docs
pnpm install
pnpm docs:dev      # 本地开发
pnpm docs:build    # 构建到 src/.vitepress/dist
pnpm docs:preview  # 本地预览构建产物
```

## 工程纪律

- 提交门禁：全量测试绿才允许 commit／push；push 前 `git fetch origin && git rebase origin/main`。
- 每个「可验收增量」一笔提交；不打 tag、不发 release、不 force push。
- 组件级许可纪律：不引入 AGPL／SSPL 组件；宽松许可组件登记进 `third_party/` 清单。
- 命名规范：代号 Memex，服务名 MemexServer，数据库与日志前缀 `memex`。

## 模块边界

Monorepo 结构与模块边界对应评审报告第四章／第五章：两套引擎各自完整、不共享通信状态，只通过共享内核交汇；服务端六模块（网关、账号与设备、组织架构、消息与归档、策略、文件元数据）。完整说明见仓库 [README](https://github.com/cuihairu/memex#readme)。
