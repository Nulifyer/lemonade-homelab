ARG UPSTREAM_IMAGE=ghcr.io/librechat-ai/librechat:latest
FROM ${UPSTREAM_IMAGE} AS patched
USER root
COPY contrib/librechat/patch-permissions.cjs contrib/librechat/test-permissions.cjs /tmp/permission-build/
RUN node /tmp/permission-build/patch-permissions.cjs /app/packages/api/dist/index.cjs && node /tmp/permission-build/test-permissions.cjs

FROM ${UPSTREAM_IMAGE}
ARG SOURCE_REVISION=unknown
LABEL org.opencontainers.image.source="https://github.com/Nulifyer/lemonade-homelab" org.opencontainers.image.revision="${SOURCE_REVISION}" org.opencontainers.image.title="LibreChat with persistent UI tool permissions"
COPY --from=patched /app/packages/api/dist/index.cjs /app/packages/api/dist/index.cjs
