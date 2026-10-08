# LibreChat compatibility image

The upstream image runs a USER MCP permission migration on every startup. It
resets UI-selected `CREATE: true` unless YAML forces that permission. This
compatibility image preserves the administrator's UI choice after the existing
`CONFIGURE_OBO` field has been migrated. Old roles still receive the original
migration once. No skills or permission overrides are assigned through YAML.

The permission fix changes one checked expression in the compiled API. It runs the actual
exported permission loader against old and migrated roles. A changed upstream
expression fails the build for review. The final image inherits the upstream
entrypoint, command, user and dependencies. It adds no startup script.
The patch must be removed when upstream makes this migration persistent.

Builds publish `ghcr.io/nulifyer/librechat-homelab:latest` and an immutable source
revision tag. Native Lemonade publication remains a separate workflow.

The pinned v0.8.8 client also omits `modelSpecs.skills` when initializing its
visible tool switches. A source patch carries that supported setting into new
chat state and preserves existing per-conversation Off choices. It also preserves
explicit settings when a submitted conversation receives its real ID. Skill
content and assignments remain owned by the web UI.

The publication workflow checks out matching upstream e8f3be0, applies the checked
patch, builds the client, runs its focused model-spec test and TypeScript check,
and checks startup/conversation loading with upstream Lighthouse. The final
container uses the matched upstream API and tested client output, without asset
rewriting at startup. Local image builds require the named build context
`--build-context chat-client=/path/to/matched/client/dist` and
`--build-context chat-api=/path/to/matched/packages/api/dist`.
The API also honors explicit Search/Skills Off choices over model-spec defaults;
those settings are tested at both UI-state and server admission boundaries.

The authenticated model catalog exposes only the Skills default, with configured
skill identities kept on the server. Its sanitizer is included in the API tests.
