# Consumer service

The Homelab fork embeds a restricted consumer listener and Wyoming voice in
`lemond`. They share its Router and backend processes. Enable `consumer.enabled`
in the shared configuration. All consumer listener settings are in the native
`consumer` section of `config.json`. Changes require a restart.

`GET service` returns the active configuration, registered role targets,
checkpoint/capability metadata and critical readiness. `GET ready` returns 200
only when every critical role resolves to a loaded, alive, healthy and pinned
backend. Both `ready` and `busy` are healthy. A missing, dead or failed backend
returns 503. An empty critical list disables automatic model startup.

Register both endpoints under `/api/v0/`, `/api/v1/`, `/v0/` and `/v1/` on the
manager and consumer listeners. The consumer also retains `/ready`, `/health`,
`/openapi.json` and `/` for existing clients. `/health` means process liveness.
Ollama retains its protocol-specific `/api/chat`, `/api/show`, `/api/tags` and
`/api/version` paths.

The consumer listener lists five roles: `small-task`, `agent-work`,
`chat-roleplay`, `speech-stt` and `speech-tts`. Persistent native aliases own their
targets. Discovery includes `alias_of`; it does not establish model readiness.
Only approved roles can invoke chat, completions, responses or speech endpoints.
Administration and lifecycle requests remain on the manager listener.

`consumer.presets` fills missing temperature, top-p, top-k, min-p, penalty and
output-limit fields. Explicit client fields win. Ollama maps the output limit
to `options.num_predict`. Backend contexts remain in native saved recipe options.
The consumer module does not replay failed requests. Native backend watchdog
recovery retains its existing policy.

When `LEMONADE_API_KEY` is configured, HTTP consumer routes require the same key.
An unset key permits LAN access. Wyoming has no authentication protocol; use a
private network. Its POSIX listener provides English Redux STT and Kokoro TTS.
Frames, clients, PCM utterances and returned WAV buffers have fixed bounds.
Malformed input and nonfinite audio are rejected. Speech disconnect cancellation
is limited by the synchronous native backend call.

The example and container contract are in
[the runtime integration guide](../../contrib/homelab/README.md).

`GET audio/voices` uses all four prefixes on both listeners. The manager returns
the backend voice names; the consumer returns English voice descriptors and the
configured default. Wyoming uses these installed IDs, languages and descriptions
and passes the caller's selected voice through to synthesis. No missing voice
is silently replaced. Home Assistant displays `description` as the voice name.
