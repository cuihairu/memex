# memex 管理后台镜像（多阶段，随 memex Docker 批交付·2026-10-11 用户令）：
#   deps   —— vcpkg 依赖层：manifest `server;admin` feature（drogon 进
#             依赖面），x64-linux 静态三件套，只有 vcpkg.json 变更才失效重装；
#   build  —— C++17 编译（服务端库＋admin，无 Qt 无测试），产出 memex_admin；
#   runtime—— ubuntu:24.04 精简运行层，非 root（uid 10001）运行。
# 与 server/Dockerfile 同款依赖口径：drogon/trantor/jsoncpp/openssl/sqlite3
# 静态链接进二进制——运行层零 .so 拖挂。www 静态页随镜像带走（无需挂载）。
FROM ubuntu:24.04 AS deps
ARG VCPKG_COMMIT=10541e317a660f4165ba4ac2851ab54a8d4577b1
# apt 镜像源可覆盖（出境受限/内网环境设 UBUNTU_MIRROR=mirrors.aliyun.com
# 之类）；空＝发行版默认 archive.ubuntu.com。
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
# vcpkg 源不走网络克隆：经 buildx named context「vcpkgsrc」注入（同
# server/Dockerfile 口径；CI 先 clone 再传入，本地传宿主检出目录）。
COPY --from=vcpkgsrc . /opt/vcpkg
RUN if [ -d /opt/vcpkg/.git ]; then \
      git -C /opt/vcpkg rev-parse --verify "${VCPKG_COMMIT}^{commit}" >/dev/null \
      && git -C /opt/vcpkg checkout --quiet "${VCPKG_COMMIT}"; \
    fi \
    && chmod +x /opt/vcpkg/bootstrap-vcpkg.sh \
    && if [ ! -x /opt/vcpkg/vcpkg ]; then \
         /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics; \
       fi
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_FORCE_SYSTEM_BINARIES=1
WORKDIR /src
COPY vcpkg.json ./
RUN /opt/vcpkg/vcpkg install --triplet x64-linux \
      --x-feature=server --x-feature=admin \
      --x-manifest-root=/src --x-install-root=/src/vcpkg_installed \
      --keep-going

FROM deps AS build
COPY CMakeLists.txt ./
COPY common ./common
COPY server ./server
COPY admin ./admin
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-linux \
      -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
      "-DVCPKG_MANIFEST_FEATURES=server;admin" \
      -DMEMEX_BUILD_CLIENT=OFF -DMEMEX_BUILD_TESTS=OFF \
    && cmake --build build --target memex_admin -j"$(nproc)"

FROM ubuntu:24.04 AS runtime
ARG UBUNTU_MIRROR=""
ENV DEBIAN_FRONTEND=noninteractive
RUN if [ -n "$UBUNTU_MIRROR" ]; then \
      sed -i "s|http://archive.ubuntu.com/ubuntu|http://$UBUNTU_MIRROR/ubuntu|g; \
              s|http://security.ubuntu.com/ubuntu|http://$UBUNTU_MIRROR/ubuntu|g" \
        /etc/apt/sources.list.d/ubuntu.sources; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
      curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home memex
COPY --from=build /src/build/admin/memex_admin /usr/local/bin/memex_admin
COPY --from=build /src/admin/www /var/lib/memex/www
USER memex
WORKDIR /var/lib/memex
# 管理面 8089（admin.memex.cuihairu.site → 192.168.5.5:8089 映射目标）
EXPOSE 8089
# 探针＝无鉴权 GET /api/whoami（起面即 401＝活着；-f 会把 401 当失败，
# 故用 -o /dev/null -w 判码）
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD [ "bash", "-c", "[ \"$$(curl -s -o /dev/null -w '%{http_code}' -m 4 http://127.0.0.1:8089/api/whoami)\" = 401 ]" ]
ENTRYPOINT ["/usr/local/bin/memex_admin"]
CMD ["--port", "8089", "--db", "/var/lib/memex/memex.db", \
     "--www", "/var/lib/memex/www"]
