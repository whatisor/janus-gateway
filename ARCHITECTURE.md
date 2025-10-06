## AudioBridge Transcription Module Architecture

### Goals
- Minimal changes to `src/plugins/janus_audiobridge.c`.
- Dynamically load/unload a custom module at runtime via `configure` messages.
- Provide real-time mixed PCM audio frames and speaker events to the module, with per-frame sequencing and timestamps for deterministic mapping.
- Keep Janus concerns separated from external processing (good modularization).

### Overview
We introduce a lightweight module ABI (in `src/plugins/janus_ab_module.h`) that external shared objects can implement to receive:
- Mixed audio frames (PCM16) from the AudioBridge mixer in real time.
- Talk state events (who starts/stops talking) derived from existing audio-level logic.

The module is dynamically managed per room: it can be loaded, reloaded, or unloaded through the existing asynchronous `configure` API. The module runs in-process and is invoked synchronously from the mixing and event paths to minimize latency, while keeping the surface area small.

### Key Components
- `src/plugins/janus_ab_module.h`
  - Defines the minimal ABI expected from a module:
    - `abmod_create(uint32_t sampling_rate, int channels, const char *config_json, const janus_abmod_callbacks *cbs, void *user)`
    - `abmod_destroy(void *ctx)`
    - `abmod_on_mix(void *ctx, const int16_t *pcm, size_t samples, uint32_t sampling_rate, int channels, uint32_t rtp_timestamp, uint64_t frame_seq, uint64_t active_talk_version)`
    - `abmod_on_event(void *ctx, const char *event_name, const char *room_id, const char *user_id, int64_t event_time_us, uint64_t talk_version)`
  - Defines optional upward callback(s) a module can use to emit events back to Janus (`emit_event`).

- `src/plugins/janus_audiobridge.c` (minimal edits)
  - Adds fields to `janus_audiobridge_room` to hold module state:
    - `abmod_lib` (dlopen handle), `abmod_ctx`, `abmod_path`, `abmod_config`.
    - Function pointers: `abmod_create`, `abmod_destroy`, `abmod_on_mix`, `abmod_on_event`.
  - Extends `configure` request validation to accept:
    - `abmod_load` (string path to .so), `abmod_unload` (bool), `abmod_config` (string JSON).
  - Implements dynamic load/unload on `configure`:
    - `dlopen` the provided path, resolve required symbols.
    - Create per-room module instance via `abmod_create` using room sampling rate and channel count (1 or 2 for spatial audio), and optional config.
    - On unload or room teardown, call `abmod_destroy` and `dlclose`.
  - Mixer hook (`janus_audiobridge_mixer_thread`):
    - Maintains `frame_seq` (uint64) incremented per frame and uses the mixer RTP `timestamp` (`rtp_timestamp`).
    - After building the mixed PCM frame (`outBuffer`), call `abmod_on_mix(ctx, outBuffer, samples, rate, channels, rtp_timestamp, frame_seq)` whenever participants are present.
  - Talk detection hook (`janus_audiobridge_participant_istalking`):
    - Maintains `talk_version` (uint64) incremented on any talk state change.
    - On state change (talking/stopped-talking), call `abmod_on_event(ctx, event, room_id_str, user_id_str, event_time_us, talk_version)`.
  - Upward events from module:
    - `janus_audiobridge_abmod_emit_event` converts module-emitted events to Janus events (`gateway->notify_event`) with optional JSON payload.
  - Clean shutdown:
    - Room free path ensures `abmod_destroy` and `dlclose` are called and memory is freed.

- Sample module `src/plugins/abmod_transcriber_template.c`
  - Demonstrates the ABI with no-op processing.
  - Tracks frames and optionally emits periodic heartbeats via `emit_event`.
  - Serves as a template for integrating real transcription engines (local or remote).

### Data Flow
1) Participants send audio → AudioBridge per-participant buffers → Mixer combines into `buffer` → `outBuffer` (PCM16) prepared each frame.
2) If a module is active for the room, the mixer invokes `abmod_on_mix` with the mixed frame and `(rtp_timestamp, frame_seq, active_talk_version)`.
3) Audio level extension updates track speak state → On change, `abmod_on_event` is invoked with `talking` / `stopped-talking`, participant identifiers, and `(event_time_us, talk_version)`.
4) The module may emit events upward via `emit_event`, which Janus forwards as plugin events (JSON) for observability/integration.

### Runtime Control via `configure`
- Load a module:
  - `{ "request": "configure", "abmod_load": "/path/to/libyourmodule.so", "abmod_config": "{...}" }`
  - Effects: unloads previous module if any; dlopens new one; creates per-room context.
- Unload module:
  - `{ "request": "configure", "abmod_unload": true }`
  - Effects: destroys context and closes the shared object.

### Concurrency and Safety
- Room mutex protects module state during load/unload and invocation setup.
- Mixer thread calls `abmod_on_mix` inline; modules must be fast/non-blocking. For heavier work, offload via internal threads/queues inside the module.
- Talk events are invoked within the talk-detection path with the room mutex held; modules should return quickly.
- Deterministic mapping: each frame carries `active_talk_version`, so offline processing can label frames without timing; timestamps are optional.

### Performance Considerations
- Zero-copy handoff of PCM16 buffer (`outBuffer`) for the current frame.
- Module is responsible for any resampling or feature extraction needed for downstream transcription.
- For remote STT services, the module should stream audio asynchronously to avoid blocking the mixer.

### Build and Packaging
- The ABI header is part of the plugin sources.
- A sample module is built as a separate shared object (`plugins/libabmod_transcriber_template.la`).
- The main audio bridge plugin links unchanged to core deps; dynamic module loading uses standard `dlopen`/`dlsym`.

### Backwards Compatibility
- If no module is configured, AudioBridge behavior is unchanged.
- All additions are optional paths gated by presence of `abmod_*` fields.

### Future Extensions
- Additional callbacks (e.g., per-participant raw audio, VAD scores) can be added to the ABI without breaking existing modules by making them optional.
- Outbound event routing to specific participants or admin channels can be added if required.
