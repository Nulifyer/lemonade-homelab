# Homelab runtime integration

This fork adds two native backends to Lemonade's existing lifecycle and model registry.
`hybrid` uses the released XDNA2/Vulkan engine and reserves both accelerators with exclusive NPU admission.
`parakeet` runs the pinned v0.6.0 Redux CPU server through the transcription interface.
The llama.cpp runtime launch hook reuses its existing request, streaming, cancellation and watchdog code.

The image bundles both engines and a Go inference/Wyoming interface. It contains no new Python serving process.
Lemonade remains the owner of Hugging Face downloads, model revisions, aliases and backend subprocesses.
The Go interface restricts consumer keys to approved aliases and inference routes.
It rejects duplicate JSON keys and ambiguous model fields. It does not execute tools or manage models.
Home Assistant uses Lemonade's existing Ollama compatibility through this interface.

The voice interface bounds connections, utterance buffers, STT and TTS admission, HTTP requests and output buffers.
It reads WAV sample rates, accepts Kokoro's bounded streaming WAV header and converts float32 samples when required.
Disconnecting a Wyoming client cancels its HTTP request. Failed POST requests are not replayed.
Clients open a fresh session after disconnects. Model restart and host recovery require deployment tests.

Configure `hybrid.npu_bin` as `/opt/llama/hybrid-server` and `parakeet.cpu_bin` as `/opt/parakeet/parakeet-server`.
Configure `llamacpp.vulkan_bin` as `/opt/llama/vulkan-server` to isolate the GPU runtime's library environment.
Each hybrid model stores `hybrid_copy_gib`, `ctx_size` and `llamacpp_args` in its recipe options.
Do not run an independent FLM or standalone hybrid stack alongside this manager's NPU workloads.

Set `LEMONADE_URL` and `AUTH_MODE=none` for LAN clients that do not use API keys.
Remove `LEMONADE_API_KEY` and `LEMONADE_ADMIN_API_KEY` from the manager environment.
Leave `LEMONADE_BACKEND_KEY` and consumer keys unset. All five model aliases become
available without credentials. Inference admission, presets and administration
route restrictions remain active. Client Authorization headers are discarded.

`AUTH_MODE=keys` (the default) requires `LEMONADE_BACKEND_KEY`, `HA_API_KEY`,
`LUNCHLOXS_API_KEY`, `ROLEPLAY_API_KEY` and `GENERAL_API_KEY`.
Consumer keys must be distinct and at least 32 characters.
The default utility voice is `af_heart`. Use the tested `TTS_VOICE` value from the deployment manifest.
`interface/presets.json` owns sampling defaults for each role. The interface fills missing
temperature, top-p, top-k, min-p and penalty fields; explicit client values win.
Ollama options use the same mappings as the OpenAI-compatible routes.
Default output limits are 256 tokens for small tasks, 1024 for roleplay and 2048
for agent work. `INFERENCE_TIMEOUT` defaults to 20 minutes and accepts 15 seconds
through 30 minutes. Set Lemonade and application deadlines to the same budget.
Wyoming has no application authentication. Keep port 10300 on the private Home Assistant network.

Verify the interface with `go test -race -timeout 60s ./...` and `go vet ./...` in `contrib/homelab/interface`.
The release image builds and tests the manager, pinned Parakeet source and Go interface before publishing.
The Homelab repository owns Portainer settings, consumer migration and hardware acceptance evidence.

## Limits

Hybrid execution accelerates eligible prefill matrix operations. Attention, recurrence and decoding remain on GPU.
MoE expert matrix operations are not accelerated by the current XDNA implementation.
Bundled backends are Linux x86_64 deployment targets. Redux processes completed utterances.
The voice interface does not supply wake-word detection, microphone drivers or speaker hardware.
