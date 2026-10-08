# LibreChat permission compatibility image

The upstream image runs a USER MCP permission migration on every startup. It
resets UI-selected `CREATE: true` unless YAML forces that permission. This
compatibility image preserves the administrator's UI choice after the existing
`CONFIGURE_OBO` field has been migrated. Old roles still receive the original
migration once. No skills or permission overrides are assigned through YAML.

The build changes one checked expression in the compiled API. It runs the actual
exported permission loader against old and migrated roles. A changed upstream
expression fails the build for review. The final image inherits the upstream
entrypoint, command, user, application and dependencies. It adds no startup script.
The patch must be removed when upstream makes this migration persistent.

Builds publish `ghcr.io/nulifyer/librechat-homelab:latest` and an immutable source
revision tag. Native Lemonade publication remains a separate workflow.
