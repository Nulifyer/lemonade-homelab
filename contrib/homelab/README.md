# Native Homelab integration

One `lemond` process owns model management, consumer HTTP, Wyoming voice and
critical-model recovery. These functions are C++ modules in Lemonade. Consumer
requests call the existing OpenAI and Ollama handlers directly. They share the
same Router, aliases, backend processes, cancellation and saved model options.
No separate interface or startup command is required.

The hybrid and Parakeet adapters remain native backend recipes. The hybrid
recipe launches the matched XDNA2/Vulkan llama.cpp bundle and reserves the NPU.
Its dynamic GGML plugin depends on the bundled llama.cpp phase-dispatch patch.
It cannot be installed into an arbitrary upstream llama.cpp binary. Parakeet
Redux uses the pinned C++ CPU runtime. Model runtimes remain subprocesses.

## Container contract

Run `ghcr.io/nulifyer/lemonade-homelab:latest`. Its entrypoint is `lemond` and its
default arguments bind the manager to `0.0.0.0:13305`. `--help` documents native
arguments. The image enables the consumer listeners through packaged defaults.
Persistent `config.json` settings override those defaults.

| Listener | Purpose |
| --- | --- |
| 13305 | Native management API, CLI and `/app` model dashboard |
| 8080 | OpenAI/Ollama role APIs, discovery, readiness and request diagnostics |
| 10300 | Private Wyoming STT/TTS for Home Assistant |

Preserve the existing configuration, Hugging Face, llama.cpp, runtime and KV
volume mounts. Device access remains `/dev/dri`, `/dev/kfd` and `/dev/accel`.
The Homelab repository owns actual mounts, resource limits and internal Traefik
routes. Publish no host ports. Keep Wyoming on the private HA network. Set `consumer.wyoming_host` to its
private Docker alias, `ai-voice`, so speech does not bind the Traefik interface.

The manager handles SIGTERM and unloads backend children. All service logs go to
stdout/stderr in one Docker container. Use its shell for native diagnostics.
The healthcheck requests consumer `/ready`, rather than inferring health from
the running process. A cold service stays unready until critical models load.

## Shared configuration

[service.example.json](service.example.json) shows the `consumer` section of
Lemonade's existing `config.json`. Native CLI/config APIs read and save these
settings. There is no second service configuration file. Use `/internal/config`
for saved shared settings and `/v1/service` for the active consumer configuration,
role targets, metadata and readiness. Consumer changes require a restart;
saved settings and the active configuration can differ until then.

The same service and readiness endpoints use all four native prefixes:
`/api/v0/`, `/api/v1/`, `/v0/` and `/v1/`. Consumer `/ready`, `/health`,
`/openapi.json` and `/` preserve the former interface's URLs. Ollama keeps its
protocol paths under `/api/`.

Consumer settings include bind addresses, ports, voice, public links, admission
limits, inference write timeout, recovery interval, critical role list and
sampling presets. Unknown settings, invalid types, conflicting listener ports
and invalid presets reject startup or configuration updates. Upstream shared
configuration behavior remains the owner of file discovery and persistence.

The defaults load and pin only `small-task`, `speech-stt` and `speech-tts`, using
persistent aliases. They currently target HA Qwen 2B, Redux and Kokoro. Recovery
uses saved recipe options and never downloads absent models. It restores missing
or dead backends and pins an existing healthy backend without reloading it.
Healthy includes `busy`. A failed role does not skip the other critical roles.
Agent Qwen3.6 35B-A3B and Skyfall remain on demand. Three LLM slots allow both alongside HA.

`consumer.presets` supplies missing sampling values. Explicit client fields win,
including Ollama top-level options. Defaults are 256 output tokens for small
tasks, 1024 for roleplay and 2048 for agent work. Temperature, top-p, top-k,
min-p and penalties are part of this shared native contract. Backend contexts
and NPU copy budgets remain saved recipe options, not HTTP sampling settings.

Leave `LEMONADE_API_KEY` and `LEMONADE_ADMIN_API_KEY` unset for the selected
key-free LAN deployment. When a native API key is set, the consumer listener
also enforces it. Wyoming has no protocol authentication and requires a private
network. The consumer listener accepts approved aliases and inference routes;
model registration, downloads and lifecycle controls belong to the manager.
The consumer module does not replay failed inference requests. Native backend
watchdog recovery retains its existing policy.

`GET /v1/audio/voices` returns installed English voice IDs, usable descriptions,
languages and the configured default. Wyoming advertises the same catalog and
forwards the selected voice to Kokoro. Voice discovery reads the running backend;
it does not invent installed voices from a static list. HA uses the voice
description as the dropdown label. OpenAI-compatible synthesis also retains
Kokoro's supported OpenAI voice aliases.

Wyoming bounds clients, frame lengths and utterance buffers. It accepts English
PCM16 input and converts finite float32 Kokoro output to PCM16. Malformed WAVs,
nonfinite samples and unsupported audio fail. Speech requests share Lemonade's
backend handling. Disconnects during a synchronous Wyoming backend call close
the session after that call returns; this is not an abort guarantee for speech.
OpenAI cancellation remains in the existing manager handlers.

## Image prompts and generation

The consumer image alias also accepts OpenAI chat completions for clients that
attach generated images through tools. Set `model: image-generation` and enable
one `generate_image` tool, optionally with LibreChat's `_mcp_` name suffix.
By default, Lemonade forwards the latest user text as the tool's `prompt` without
an LLM. Set `consumer.image_prompt_model: chat-roleplay` to enable the creative
writer. It uses the roleplay alias, a structured JSON
response and a 1,024-token output limit. The writer produces the prompt only.
Lemonade constructs the tool call itself. It does not require the writer to
support tool calls or vision. No HA or agent LLM participates.
Writer temperature defaults to 0.3 independently of roleplay chat. Explicit chat
request sampling fields override writer defaults. Other omitted sampling fields
use the roleplay preset. The writer output budget remains separate from the
client's synthetic tool-call response budget.

`consumer.image_prompt_instructions` supplies the writer's editable instructions.
The default preserves scene, character identity, gender, clothing, actions,
relative positions and intent. Latest explicit roleplay state takes precedence
over older descriptions. Compatible visual details fill unspecified gaps.
Output formatting remains owned by the service. It appends the JSON protocol
and accepts a prompt up to 6,000 bytes. Saved instruction changes use
`POST /internal/set` on the manager and require a service restart. They are visible
in `/v1/service`. SillyTavern owns its separate contextual writer templates in
Image Generation > Image Prompt Templates. Raw generation bypasses both writers.

The chat request's `image_prompt_mode` can be `creative` or `direct`. When the
writer is configured, creative is the default. Explicit direct mode bypasses
the writer. `POST /v1/images/generations` and the SD-compatible API always use
the provided prompt directly. Discovery distinguishes these raw generation
interfaces from the chat interface and reports the configured prompt model.
Malformed or incomplete writer results fail before generation. They are not
silently rewritten or replayed. The direct path remains available independently.
Neither path has a separate content classifier or safety checker.

Streaming emits standard
OpenAI tool-call chunks. After the matching tool result, it confirms success or
reports failure without submitting the job again. Image artifacts projected into
synthetic user messages and tool-budget notices remain result context. They do
not start new jobs. This mode accepts text image
requests, not general conversation or image editing. The existing image job
limits, cancellation, memory admission and backend cleanup still apply.

Managed diffusion checkpoints use `main` plus optional `vae`, `text_encoder`
for Qwen encoders, `t5xxl`, `clip_l` and `clip_g`. Split models require a VAE.
Every declared component must exist before the adapter launches. These paths
come from Lemonade's model cache and cannot be replaced through custom runtime
arguments. Chroma therefore uses `--t5xxl`, while existing Qwen pipelines retain
`--llm`. Bundled SDXL checkpoints retain `-m`.

## Image request controls

Explicit client values take precedence over saved model defaults. Raw OpenAI
requests, the SD-compatible adapter and the image MCP tool accept steps, CFG,
size, seed, negative prompt, sampler, scheduler, flow shift and CLIP skip.
OpenAI/MCP use `n` for image count; the SD adapter uses `batch_size` and returns
all images. Image chat dispatch accepts these controls in `image_options`,
with the chat message supplying the prompt. Legacy `image_size` remains supported;
conflicting size values return an error.

The service fallback is 512×512 and one image. Sampling fields stay absent when
omitted, so the native diffusion adapter resolves the selected model's options.
There is no default eight-step, CFG-4, 1024-pixel or one-image cap. The legacy
`consumer.image_max_steps` defaults to zero, meaning no operator step cap. A
positive value is an explicit operator policy, not a sampling default. Existing
saved configuration takes precedence; replace an old value of eight with zero
to remove that policy. Image jobs default to a 1,200-second timeout.

Discovery reports available controls, effective model defaults and any operator
step cap. OpenAPI and MCP use the same option schema and validation. Positive
runtime integer ranges, finite numeric values and supported sampler names are
validated. Model-specific dimension alignment and hardware capacity still apply.
Runtime control markup is not accepted inside image prompts. Request-size,
concurrency, memory admission and cancellation remain service policies.

## Build, tests and upstream updates

The production base retains Ubuntu 26.04, glibc, XRT and Vulkan. Alpine uses musl
and cannot directly load these matched native binaries. Build stages contain the
compilers, npm and tests; the final image omits them. The former Go interface
build and binary are removed. The accepted v0.1.13 image baseline was 0.481 GiB;
new size and timings require measurements.

Release builds use a shared GHCR `buildcache` tag for intermediate layers.
[Docker registry cache](https://docs.docker.com/build/ci/github-actions/cache/#registry-cache)
allows reuse across independent release tags. Publication is serialized and
requires native tests and a default-container smoke test before pushing `latest`.
The smoke test does not establish accelerator or real-model correctness.

Run the focused native checks with:

```bash
cmake --build build-local -j4 --target lemond test_consumer_service test_homelab_ollama
ctest --test-dir build-local --output-on-failure -R '^(ConsumerServiceTest|HomelabOllamaTest)$'
```

`ConsumerServiceTest` exercises configuration, explicit sampling, aliases,
restricted routes, stream admission, busy readiness, partial recovery, real
Wyoming sockets, PCM/float32 conversion, voice selection and shutdown with an idle voice client.
The Homelab repository owns hardware, actual-model, HA and LibreChat acceptance.
Wyoming currently uses POSIX sockets; disabled consumer support still compiles
on Windows. An enabled Windows Wyoming listener fails explicitly.

Keep the consumer module separate from backend implementations. Upstream merges
should need only its Server lifecycle/handler registration, shared configuration
validation and source/test registration. Runtime ABI changes remain in the
separate `llama-xdna-hybrid` repository and require matched compatibility tests.

Inherited upstream triage, publishing and hardware-runner jobs are gated to
`lemonade-sdk/lemonade` and disabled in this fork. Fork workflows own hosted
native checks, release builds and promotion. No account notification settings
are changed.

## Resource budgets and revisions

The dashboard RAM meter opens resource budgets and model revision status.
`GET /api/v1/system-stats` includes Linux `resource_budget`: host available RAM,
cgroup current/peak/limits, and per-runtime CPU seconds, process PSS and DRM
resident GPU/GTT memory. Duplicate descriptors count once. Missing counters
remain null; NPU utilization is device-wide. These overlapping memory views
must not be added. Driver memory can be outside the cgroup charge.

Model revisions are checked at startup when `auto_check_model_updates` is true.
The dialog can run `POST /api/v1/models/check-updates` and list downloaded
models with newer revisions. Checks never download weights; automatic model
updates remain disabled. Existing download controls apply an update explicitly.

## Consumer tools

The consumer listener exposes stateless Streamable HTTP MCP at `/mcp`. Its
read-only tools inspect model services, critical readiness and installed voices.
`consumer.documents` optionally supplies named reviewed runbook text. Each text
is limited to 16 KiB, with 16 names and 64 KiB total. `read_runbook` accepts only
those names. It cannot read host files, run commands, change models or download
weights. Runbooks describe configured design, not current infrastructure state.
The manager's upstream `/mcp` remains a separate, broader inference interface.
Connect ordinary chat clients to the consumer MCP listener.

## Compact images

The consumer `image-generation` alias serves `POST /v1/images/generations`.
It accepts a plain prompt of at most 2000 bytes, one base64 image, configured
`image_size` (256x256, 512x512, 768x768 or 1024x1024), `steps`
(1 through `image_max_steps`, at most 8), and an optional integer seed. Backend control tags and extra options are
rejected. One image job can run at a time. `image_min_available_gib` defaults
to 12 GiB of host available RAM before admission. This is a snapshot guard,
not a reservation. Unknown platform counters do not invent a budget.

The image-only MCP endpoint `/mcp/images` supplies `generate_image`, returning
a PNG content block. Its initialize response assigns an opaque `Mcp-Session-Id`.
Clients retain that header for subsequent requests. `notifications/cancelled`
sets the matching request cancellation flag in that session; equal IDs from
other sessions remain isolated. DELETE terminates the session and cancels its
active jobs. There are at most 64 sessions, with idle expiry after 15 minutes.
An expired session returns 404 so the client can initialize again. No replay or
server-to-client SSE stream is offered. The read-only `/mcp` stays stateless.

The image endpoint uses the same admission and API policy. The native
Router unloads the image child after every consumer job, including a disconnect,
while text and speech runtimes remain loaded. Images are never critical startup
models. Manager requests retain normal explicit model-management behavior.

## Hybrid vision and load failures

A hybrid model with a resolved matching `mmproj` supports image input. The
projector runs on CPU because the bundled hybrid device setup currently aborts
when the image encoder is offloaded. Text uses the existing XDNA prefill and
Vulkan decode path. Vision is hidden when no projector resolves, and speculative
decoding remains disabled. No per-model launch flag is required.

With `auto_evict: false`, a failed backend load returns its error without
unloading other resident models. Critical startup recovery remains independent.

## SillyTavern image API

The consumer listener supports `POST /sdapi/v1/txt2img` for the native
stable-diffusion.cpp client in SillyTavern. This fixed protocol path is separate
from the manager's versioned routes. It adapts to the same image-generation
role, memory budget, single-job limit, timeout and runtime release policy as
`/v1/images/generations`. The response contains a base64 `images` array.
The service defaults to one 512×512 image. Explicit client sizes override
`consumer.image_size`; that setting is a fallback, not a fixed-size policy.
Each dimension must be 256 through 1024 in multiples of 64. Portrait and
landscape requests use the same bound. The image MCP tool accepts optional
`size: "WIDTHxHEIGHT"`. Image chat dispatch accepts optional `image_size` and
forwards it into the tool call without interpreting dimensions in prompt content.
When `steps` is omitted, the consumer reads the selected model's effective
recipe options, then its image defaults. Explicit caller steps take precedence.
The same maximum-step bound applies to defaults and overrides. Legacy metadata
without step settings retains the four-step fallback.
Explicit supported `sampler_name` and `scheduler` selections take precedence
over model defaults. Omitted, empty and `N/A` selections use model defaults.
The adapter preserves actual names; it does not map Euler to iPNDM or discrete
to beta. SillyTavern's sd.cpp mode offers iPNDM but omits beta from its hard-coded
scheduler list. Its tested Chroma profile uses iPNDM/discrete, eight steps and
CFG 1 at 512×512. Other clients can use the saved iPNDM/beta model preset.
Other supported API controls remain bounded. The OPTIONS image probe is read-only.

The homelab container includes the tested Linux Vulkan sd.cpp release
`master-843-462d675` under `/opt/sdcpp/vulkan`. The build verifies the official
archive SHA-256 and binary source identity. `sdcpp.vulkan_bin` selects that
read-only runtime through the existing configuration contract. Persisted settings
override image defaults, so update a saved `builtin` selection during migration.
This keeps the runtime reproducible with the image and preserves existing runtime
and model caches. OCI labels record the release and archive hash. Other platforms
and backends keep their existing install configuration.
