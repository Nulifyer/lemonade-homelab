# T3/Pi client service

This image runs the official T3 0.1.0 Linux x64 release and Pi 0.80.5 on
Node 22.22.1. The T3 archive SHA-256 is checked during build. The image includes
T3's MIT notice and the Debian libraries named by its browser setup code.
It needs no GPU devices, model weights or Docker socket.

Lemonade owns inference at `http://lemonade:8080/v1`. Pi exposes the stable
`agent-work` and `small-task` aliases. The general profile declares 65,536
context tokens and an 8,192-token output ceiling. Verify 65,536 effective context
per slot in the actual model service before deployment. The seeded small profile
uses 8,192 context, 256 output tokens and no thinking.

Docker initializes a new `/data` volume with the T3 provider settings and Pi
configuration. Existing data is never overwritten by a startup script. T3 state
lives under `/data/t3`; Pi configuration, auth and sessions use `/data/pi`.
Repositories use the separate `/workspace` volume. The service runs as UID 1000.
Pi loads `/opt/t3/lemonade.mjs` through its supported extension setting. This
translates its thinking selector without changing sampling. Nex templates need
`HOMELAB_REASONING_TEMPLATE=effort`; other tested Qwen templates use the default.
The model service owns the normal sampling and thinking budget.

The image starts HTTP/WebSocket on 3773. Its healthcheck proves HTTP startup;
model inference, provider discovery, pairing and browser operation require their
own deployment checks. Generate a fresh pairing link with `t3 pair --label klapt`
in the container console. Pair klapt directly with the reachable Mage address.
Keep existing kdesk connections and histories. Never save pairing credentials in
repository files or public build logs.

The browser downloads into T3's persistent home on first use. Chrome's sandbox
remains enabled by default. Container/host support must be checked before claiming
browser availability. T3 documents `t3 browser setup` for missing prerequisites.
