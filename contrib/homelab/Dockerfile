FROM ghcr.io/nulifyer/llama-xdna-hybrid@sha256:982a586fdd49bfd181b8e4613ded8d8e857969bf650ecb44ee74c1120000118d AS hybrid-runtime

# Bare Ubuntu lacks OpenSSL CA symlinks; give APT the copied bundle explicitly.
FROM ubuntu:26.04 AS manager-build
ARG DEBIAN_FRONTEND=noninteractive
COPY --from=hybrid-runtime /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://security.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g; s|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt -o APT::Update::Error-Mode=any update && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt -o APT::Update::Error-Mode=any install -y --no-install-recommends build-essential cmake ninja-build libssl-dev pkg-config libdrm-dev libcurl4-openssl-dev libzstd-dev git python3 nodejs npm && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt README.md LICENSE ./
COPY cmake/ ./cmake/
COPY src/ ./src/
COPY docs/ ./docs/
COPY test/ ./test/
COPY data/ ./data/
COPY tools/ ./tools/
ARG SERVICE_VERSION=development
RUN printf '2026.42.0-%s\n' "$SERVICE_VERSION" > .version && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_WEB_APP=ON -DBUILD_TAURI_APP=OFF && cmake --build build -j4 --target lemond lemonade web-app test_backend_mode_contract test_recipe_options_precedence test_recipe_arg_resolution test_request_cancellation test_process_manager test_homelab_ollama test_model_download_state test_consumer_service test_resource_budget test_sdcpp_managed_models && ctest --test-dir build --output-on-failure -R '^(BackendModeContractTest|RecipeOptionsPrecedence|RecipeArgResolutionTest|RequestCancellationTest|ProcessManagerTest|HomelabOllamaTest|ModelDownloadStateTest|ConsumerServiceTest|ResourceBudgetTest|SdcppManagedModelsTest)$' && python3 test/homelab_native_service.py build/lemond && python3 test/homelab_model_metadata.py build/lemond && node test/homelab_recipe_options.cjs

FROM ubuntu:26.04 AS speech-build
ARG DEBIAN_FRONTEND=noninteractive
COPY --from=hybrid-runtime /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://security.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g; s|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt -o APT::Update::Error-Mode=any update && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt -o APT::Update::Error-Mode=any install -y --no-install-recommends build-essential cmake git ca-certificates && rm -rf /var/lib/apt/lists/*
RUN git clone --recursive https://github.com/mudler/parakeet.cpp.git /src && cd /src && git checkout da8bb4c45d08e081e759849bc6ee0a38720eb0b9 && git submodule update --init --recursive
WORKDIR /src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF -DPARAKEET_BUILD_SERVER=ON -DPARAKEET_BUILD_TESTS=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure -LE model && mkdir -p /out/bin /out/lib && cp build/examples/server/parakeet-server /out/bin/ && find build -name '*.so*' -exec cp -a '{}' /out/lib/ ';'

FROM ubuntu:26.04 AS image-runtime
ARG DEBIAN_FRONTEND=noninteractive
COPY --from=hybrid-runtime /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
RUN sed -i 's|http://security.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g; s|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources && apt-get -o Acquire::Retries=3 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt update && apt-get -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt install -y --no-install-recommends curl unzip ca-certificates && rm -rf /var/lib/apt/lists/*
RUN curl --fail --location --retry 3 https://github.com/leejet/stable-diffusion.cpp/releases/download/master-843-462d675/sd-master-462d675-bin-Linux-Ubuntu-24.04-x86_64-vulkan.zip -o /tmp/sd.zip && printf '%s  %s\n' 366d8e59319a01f2f3a822b956fb29753983a7d4ada229cb682eec36a9496b40 /tmp/sd.zip | sha256sum -c - && mkdir -p /out && unzip -q /tmp/sd.zip -d /out && rm /tmp/sd.zip && printf '%s  %s\n' 38939323c3e4ca7127af9cf0017410e884b12aae29f6834545a1ca093e00ce54 /out/sd-server f60fb0b9b974319e09b82f26f20cb43bb0f9b154bc1f886e4045bce7a7e2c163 /out/libstable-diffusion.so | sha256sum -c -

FROM hybrid-runtime
USER root
ARG DEBIAN_FRONTEND=noninteractive
ARG SOURCE_REVISION=unknown
ARG SERVICE_VERSION=development
LABEL org.opencontainers.image.source="https://github.com/Nulifyer/lemonade-homelab" org.opencontainers.image.revision="${SOURCE_REVISION}" org.opencontainers.image.version="${SERVICE_VERSION}" org.opencontainers.image.title="Lemonade AI service" org.opencontainers.image.description="Managed GPU/NPU models, inference APIs and English voice" org.opencontainers.image.documentation="https://github.com/Nulifyer/lemonade-homelab/blob/homelab-runtime/contrib/homelab/README.md"
LABEL io.lemonade.sdcpp.version="master-843-462d675" io.lemonade.sdcpp.archive.sha256="366d8e59319a01f2f3a822b956fb29753983a7d4ada229cb682eec36a9496b40"
RUN sed -i 's|http://security.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g; s|http://archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt -o APT::Update::Error-Mode=any update && apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Acquire::https::CAInfo=/etc/ssl/certs/ca-certificates.crt -o APT::Update::Error-Mode=any install -y --no-install-recommends libcurl4t64 libdrm2 unzip xz-utils zstd libreadline8t64 && rm -rf /var/lib/apt/lists/* && mkdir -p /opt/lemonade/.config/lemonade /opt/lemonade/.cache/huggingface /opt/lemonade/.cache/lemonade /opt/lemonade/llama /opt/lemonade/kv /run/lemonade && chown -R 10001:10001 /opt/lemonade /run/lemonade
COPY --from=manager-build /src/build/lemond /src/build/lemonade /opt/lemonade/
COPY --from=manager-build /src/build/resources/ /opt/lemonade/resources/
COPY --from=speech-build /out/ /opt/parakeet/
COPY --from=image-runtime /out/ /opt/sdcpp/vulkan/
COPY contrib/homelab/container-defaults.json /opt/lemonade/service-defaults.json
COPY --chmod=755 contrib/homelab/vulkan-server /opt/llama/vulkan-server
RUN printf '#!/bin/sh\nexport LD_LIBRARY_PATH=/opt/parakeet/lib\nexec /opt/parakeet/bin/parakeet-server "$@"\n' > /opt/parakeet/parakeet-server && chmod 755 /opt/parakeet/parakeet-server
ENV LEMONADE_DEFAULTS_PATH=/opt/lemonade/service-defaults.json HOME=/opt/lemonade XDG_RUNTIME_DIR=/run/lemonade PATH=/opt/lemonade:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LD_LIBRARY_PATH="" GGML_BACKEND_PATH=""
WORKDIR /opt/lemonade
USER 10001:10001
EXPOSE 13305 8080 10300
HEALTHCHECK --interval=15s --timeout=7s --start-period=120s --retries=8 CMD ["curl", "--fail", "--silent", "--max-time", "6", "http://127.0.0.1:8080/ready"]
ENTRYPOINT ["/opt/lemonade/lemond"]
CMD ["--host", "0.0.0.0"]
