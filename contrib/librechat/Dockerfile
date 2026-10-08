ARG UPSTREAM_IMAGE=ghcr.io/librechat-ai/librechat:v0.8.8
FROM ${UPSTREAM_IMAGE} AS patched
USER root
COPY --from=chat-api / /app/packages/api/dist/
COPY contrib/librechat/patch-permissions.cjs contrib/librechat/test-permissions.cjs /tmp/permission-build/
RUN node /tmp/permission-build/patch-permissions.cjs /app/packages/api/dist/index.cjs && node /tmp/permission-build/test-permissions.cjs

FROM ${UPSTREAM_IMAGE}
ARG SOURCE_REVISION=unknown
LABEL org.opencontainers.image.source="https://github.com/Nulifyer/lemonade-homelab" org.opencontainers.image.revision="${SOURCE_REVISION}" org.opencontainers.image.title="LibreChat with persistent UI permissions and model-spec Skills defaults"
COPY --from=patched /app/packages/api/dist/ /app/packages/api/dist/

# The named context contains the matched, source-built and tested web client.
COPY --from=chat-client / /app/client/dist/
