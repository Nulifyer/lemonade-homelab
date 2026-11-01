FROM node:22.22.1-bookworm-slim

ARG T3_VERSION=0.1.0
ARG T3_ARCHIVE_SHA256=f7703ff1bb8f6243580058b3ccf2c02abb62eba4b991e26bbb1da22f4b56d9b1
ARG PI_VERSION=0.80.5


# Browser libraries follow the pinned T3 browser setup list for Debian 12.
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash ca-certificates curl git openssh-client python3 ripgrep fonts-liberation libatomic1 \
    libnss3 libglib2.0-0 libatk1.0-0 libatk-bridge2.0-0 libatspi2.0-0 \
    libdbus-1-3 libx11-6 libxcb1 libxcomposite1 libxdamage1 libxext6 \
    libxfixes3 libxrandr2 libxkbcommon0 libgbm1 libasound2 libexpat1 \
    && rm -rf /var/lib/apt/lists/*
RUN curl --fail --location --retry 3 --silent --show-error \
      "https://github.com/pingdotgg/t3code/releases/download/v${T3_VERSION}/t3-${T3_VERSION}-linux-x64.tar.gz" \
      -o /tmp/t3.tar.gz \
    && printf '%s  /tmp/t3.tar.gz\n' "$T3_ARCHIVE_SHA256" | sha256sum --check --status \
    && mkdir -p /opt/t3 \
    && tar -xzf /tmp/t3.tar.gz --strip-components=1 -C /opt/t3 \
    && rm /tmp/t3.tar.gz \
    && /opt/t3/t3 --version \
    && npm install -g --ignore-scripts --no-audit --no-fund "@earendil-works/pi-coding-agent@${PI_VERSION}" \
    && npm cache clean --force

COPY contrib/homelab/t3/LICENSE.t3code /opt/t3/LICENSE.t3code
COPY contrib/homelab/t3/lemonade.mjs /opt/t3/lemonade.mjs
RUN mkdir -p /data/pi /data/t3/userdata /workspace && chown -R node:node /data /workspace
COPY --chown=node:node contrib/homelab/t3/pi-models.json /data/pi/models.json
COPY --chown=node:node contrib/homelab/t3/pi-settings.json /data/pi/settings.json
COPY --chown=node:node contrib/homelab/t3/t3-settings.json /data/t3/userdata/settings.json

ENV T3CODE_HOME=/data/t3 \
    PI_CODING_AGENT_DIR=/data/pi \
    T3CODE_TELEMETRY_ENABLED=false \
    PATH=/opt/t3:${PATH}
USER node
WORKDIR /workspace
VOLUME ["/data", "/workspace"]
ARG SOURCE_REVISION=development
ARG SERVICE_VERSION=development
LABEL org.opencontainers.image.source="https://github.com/Nulifyer/lemonade-homelab" \
      org.opencontainers.image.revision=$SOURCE_REVISION \
      org.opencontainers.image.version=$SERVICE_VERSION
EXPOSE 3773
HEALTHCHECK --interval=30s --timeout=5s --start-period=30s \
    CMD node -e "fetch('http://127.0.0.1:3773/',{signal:AbortSignal.timeout(3000)}).then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
ENTRYPOINT ["/opt/t3/t3"]
CMD ["serve", "--host", "0.0.0.0", "--port", "3773", "--auto-bootstrap-project-from-cwd", "/workspace"]
