# memex 服务端镜像（多阶段）：
#   deps   —— vcpkg 依赖层：manifest（vcpkg.json，默认 feature＝服务端依赖、无
#             Qt）按 x64-linux 静态三件套装，只有 vcpkg.json 变更才失效重装；
#   build  —— C++17 编译（仅服务端，MEMEX_BUILD_CLIENT/TESTS=OFF），产出
#             自包含单二进制；
#   runtime—— ubuntu:24.04 精简运行层，非 root（uid 10001）运行。
# 依赖静态/动态口径（README 部署节同步）：asio、nlohmann-json 头文件即用；
# protobuf、openssl（libcrypto）、sqlite3、aws-sdk-cpp（s3）静态链接进二进制
# ——运行层零 .so 拖挂。宿主开发树走 x64-linux-dynamic（Qt 客户端同树要求
# 动态），容器构建与宿主构建互不相干。
FROM ubuntu:24.04 AS deps
ARG VCPKG_COMMIT=10541e317a660f4165ba4ac2851ab54a8d4577b1
# apt 镜像源可覆盖（出境受限/内网环境设 UBUNTU_MIRROR=mirrors.aliyun.com
# 之类）；空＝发行版默认 archive.ubuntu.com。https 镜像要先有 CA（基础镜像
# 没有），故约定 http:// 形式填入。
ARG UBUNTU_MIRROR=""
ENV DEBIAN_FRONTEND=noninteractive
RUN if [ -n "$UBUNTU_MIRROR" ]; then \
      sed -i "s|http://archive.ubuntu.com/ubuntu|http://$UBUNTU_MIRROR/ubuntu|g; \
              s|http://security.ubuntu.com/ubuntu|http://$UBUNTU_MIRROR/ubuntu|g" \
        /etc/apt/sources.list.d/ubuntu.sources; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git curl zip unzip tar pkg-config \
      ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*
# vcpkg 源不走网络克隆：经 buildx named context「vcpkgsrc」注入（本地
# --build-context vcpkgsrc=<检出路径>，CI 先 clone 再传入）。注入的快照
# 用 git 校验钉在与 vcpkg.json builtin-baseline 相同的 commit（宿主同款
# 快照）；快照已带 bootstrap 出的 vcpkg 二进制则直接用（离线），否则
# 回退 bootstrap 下载 vcpkg-tool（CI 克隆的干净检出走这条）。
# 快照带 downloads/ 缓存（源码包）时 vcpkg install 全程离线可装。
COPY --from=vcpkgsrc . /opt/vcpkg
RUN git -C /opt/vcpkg rev-parse --verify "${VCPKG_COMMIT}^{commit}" >/dev/null \
    && git -C /opt/vcpkg checkout --quiet "${VCPKG_COMMIT}" \
    && if [ ! -x /opt/vcpkg/vcpkg ]; then \
         /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics; \
       fi
ENV VCPKG_ROOT=/opt/vcpkg
# 强制用发行版工具（ninja/cmake 等），挡掉 vcpkg 钉版本下载（ninja 要
# 1.13.2、发行版 1.11.1 不过闸就会去 github 下载——离线/受限网络必踩）
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
WORKDIR /src
COPY vcpkg.json ./
RUN /opt/vcpkg/vcpkg install --triplet x64-linux \
      --x-manifest-root=/src --x-install-root=/src/vcpkg_installed \
      --keep-going

FROM deps AS build
COPY CMakeLists.txt ./
COPY common ./common
COPY server ./server
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-linux \
      -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
      -DMEMEX_BUILD_CLIENT=OFF -DMEMEX_BUILD_TESTS=OFF \
    && cmake --build build --target memex_server -j"$(nproc)"

FROM ubuntu:24.04 AS runtime
ARG UBUNTU_MIRROR=""
ENV DEBIAN_FRONTEND=noninteractive
# curl：HEALTHCHECK 探针 + 运维现场；ca-certificates：S3 over TLS
RUN if [ -n "$UBUNTU_MIRROR" ]; then \
      sed -i "s|http://archive.ubuntu.com/ubuntu|http://$UBUNTU_MIRROR/ubuntu|g; \
              s|http://security.ubuntu.com/ubuntu|http://$UBUNTU_MIRROR/ubuntu|g" \
        /etc/apt/sources.list.d/ubuntu.sources; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
      curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home memex
COPY --from=build /src/build/server/memex_server /usr/local/bin/memex_server
USER memex
WORKDIR /var/lib/memex
VOLUME ["/var/lib/memex"]
# 协作面 24360／webhook 接入 24361／文件面 24561
EXPOSE 24360 24361 24561
# 探针＝文件面无鉴权 GET /files/health（起面即 200）。改文件面端口时同步
# 设 MEMEX_FILES_PORT（HEALTHCHECK 不随 CMD 覆盖而变）
HEALTHCHECK --interval=30s --timeout=5s --start-period=30s --retries=3 \
  CMD curl -sf "http://127.0.0.1:${MEMEX_FILES_PORT:-24561}/files/health" || exit 1
ENTRYPOINT ["/usr/local/bin/memex_server"]
CMD ["serve", "--db", "/var/lib/memex/memex.db", \
     "--port", "24360", "--webhook-port", "0", "--files-port", "24561"]
