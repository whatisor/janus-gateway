# Transcriber ABMod Design (Unified, provider-based)

## Goal

Provide a single AudioBridge extension module that can transcribe:

- **Per-participant PCM** (default) via `abmod_on_participant_pcm`
- **Mixed PCM** (optional) via `abmod_on_mix` when enabled

Transcription is provider-modular:

- Default provider: **AWS** (`"aws"`)
- Optional provider: **OpenAI** (`"openai"`)

## Scope / Simplification

- Extend AB module ABI with optional per-participant PCM callback.
- Keep existing mixed-audio transcriber behavior unchanged.
- Keep only one runtime-loadable module:
  - `libabmod_transcriber_template.la` (historical name; now the real transcriber)
- Providers are behind `abmod_provider_*` and selected via `abmod_config.provider`.

## Non-goals

- Replacing current merged-audio module.
- Changing AudioBridge client signaling protocol (`abmod_load`, `abmod_unload`, `abmod_config` remain unchanged).
- Implementing all STT providers at once.

## Architecture

### 1) Audio source tap in AudioBridge

`janus_audiobridge_participant_thread` invokes optional:

- `abmod_on_participant_pcm(ctx, room_id, user_id, pcm, samples, rate, channels, rtp_ts, frame_seq, talk_version)`

This callback receives decoded participant PCM frames and allows per-user processing without touching mixer logic.

### 2) Module ABI

ABI header: `src/plugins/janus_ab_module.h`

- Existing required symbols remain:
  - `abmod_create`
  - `abmod_destroy`
  - `abmod_on_mix`
- Existing optional symbol remains:
  - `abmod_on_event`
- New optional symbol:
  - `abmod_on_participant_pcm`

Backward compatibility: old modules that do not export `abmod_on_participant_pcm` still load and run.

### 3) Unified module (per-user default)

Module: `src/plugins/abmod_transcriber_template.c`

- Maintains active stream map keyed by `room_id|user_id`.
- Owns a bounded internal work queue plus a dedicated worker thread.
- `abmod_on_participant_pcm(...)` only copies/enqueues PCM work and returns immediately.
- `abmod_on_event(...)` only enqueues control work such as `stopped-talking`.
- Worker thread opens a provider stream on first queued PCM for a `(room_id,user_id)` pair.
- Worker thread pushes queued PCM to provider adapter.
- Worker thread closes the provider stream on queued `stopped-talking`.
- `abmod_on_mix(...)` is optional:
  - Disabled by default (`enable_mix=false`)
  - When enabled (`enable_mix=true`), per-user PCM streaming via `abmod_on_participant_pcm` is **disabled** — the two modes are mutually exclusive.
  - In mix mode, mixed PCM is streamed under `user_id="mixed"` and uses the most recently observed `room_id` (since the mix callback does not include room identity).
- Emits Janus events:
  - `transcription.partial`
  - `transcription.final`
  - `transcription.error`

### 4) Provider abstraction

Header: `src/plugins/abmod_provider.h`

Interface:

- `abmod_provider_create(...)`
- `abmod_provider_destroy(...)`
- `abmod_provider_open_user_stream(...)`
- `abmod_provider_send_pcm(...)`
- `abmod_provider_close_user_stream(...)`

Callbacks to module:

- transcript callback (partial/final)
- error callback

### 5) AWS provider adapter

Implementation: `abmod_provider_aws.c` (configuration and stream registry) plus `abmod_provider_aws_sdk.cpp` (official **AWS SDK for C++** behind a small C API in `abmod_provider_aws_sdk.h`).

- Build Janus with `--enable-abmod-aws-sdk` and `--with-aws-sdk-cpp=PREFIX` (or system-installed `libaws-cpp-sdk-transcribestreaming` / `libaws-cpp-sdk-core`). Without that flag, a stub is linked and streams will not start.
- One **StartMedicalStreamTranscription** session per `room_id|user_id`; PCM is sent as `AudioEvent` frames on the bidirectional stream (same service as the old websocket endpoint, but via the SDK).
- Credentials: default AWS provider chain (environment, shared config, IAM role, etc.); optional explicit keys via `abmod_config` or `AWS_*` env vars.
- Multi-channel input is mixed down to mono in the native layer before sending.

## Configuration Contract (`abmod_config`)

`abmod_config` is JSON string passed to `abmod_create`.

Supported keys:

- `provider` (currently `aws`)
- `aws_language_code` (default `en-US`; AWS Medical Streaming only supports `en-US` — the SDK layer hardcodes this regardless of the configured value)
- `aws_region` (default `us-east-1`)
- `aws_specialty` (default `PRIMARYCARE`; the SDK layer currently hardcodes `PRIMARYCARE` regardless of this value — extend `parse_specialty()` in `abmod_provider_aws_sdk.cpp` to support other values)
- `aws_stream_type` (default `CONVERSATION`; or `DICTATION`)
- `aws_session_id_prefix` (default empty string; final session id is `<prefix><room_id>-<user_id>`)
- `aws_medical_redaction` (default `false`)
- `aws_access_key_id` (optional override; prefer environment variable)
- `aws_secret_access_key` (optional override; prefer environment variable)
- `aws_session_token` (optional; for temporary credentials)

Legacy keys `aws_ws_url`, `aws_sigv4_expires`, and `aws_query_overrides` are ignored (kept for backward-compatible configs).

Example:

```json
{
  "provider": "aws",
  "aws_language_code": "en-US",
  "aws_region": "us-east-1",
  "aws_specialty": "PRIMARYCARE",
  "aws_stream_type": "CONVERSATION",
  "aws_session_id_prefix": "prod-",
  "aws_medical_redaction": false
}
```

Credential resolution order (higher number wins):

1. Environment variables on Janus process/container:
   - `AWS_ACCESS_KEY_ID`
   - `AWS_SECRET_ACCESS_KEY`
   - `AWS_SESSION_TOKEN` (optional)
2. Explicit values in `abmod_config` (override env vars when non-empty):
   - `aws_access_key_id`
   - `aws_secret_access_key`
   - `aws_session_token`
3. AWS SDK default provider chain (used when neither env vars nor config supply credentials): shared `~/.aws/credentials`, IAM instance/task role, etc.

Production recommendation: use environment variables or IAM role credentials injection rather than embedding secrets in `abmod_config`.

## Event Contract (Module -> Janus -> Clients)

Envelope remains unchanged from AudioBridge AB module path:

- `{ "audiobridge": "abmod", "event": "<event_name>", "payload": { ... } }`

Payload fields emitted by per-user module:

- `provider`
- `room_id`
- `user_id`
- `user` (compat alias of `user_id`)
- `language`
- `text`
- `transcript` (compat alias of `text`)
- `item_id` (stable user-based key)
- `type` (`partial`, `final`, `error`, `auth_error`)
- `ts_us`

Errors include:

- `message`
- `text` (compat alias)
- `type` (`error` or `auth_error`)

## Threading and Performance

- AudioBridge decode thread remains non-blocking:
  - callback only copies PCM into the module queue and never performs provider/network work inline.
- Per-user module owns one background worker thread per loaded module instance:
  - stream open/send/close operations are executed only on this worker thread.
- AWS adapter uses dedicated background thread per active user stream.
- Backpressure strategy:
  - module queue is bounded.
  - PCM may be dropped if the queue is saturated.
  - control events such as `stopped-talking` should still be admitted preferentially so stream-close work is not starved by audio backlog.
  - provider queue buffers PCM chunks; no network I/O on decode thread.
- Reconnect strategy:
  - No automatic reconnection. When a stream encounters a write error, it is marked broken and closed. The next PCM frame for that `(room_id, user_id)` pair will trigger a new `open_user_stream` call.

## Build and Deployment

Build integration: `src/Makefile.am`

- Added:
  - `plugins/libabmod_transcriber_template.la`

Runtime loading still uses AudioBridge `configure` API:

- `abmod_load`
- `abmod_unload`
- `abmod_config`

## Compatibility and Rollout

- The unified module is opt-in by `abmod_load`.
- If a module does not export the optional callback symbol `abmod_on_participant_pcm`, AudioBridge behavior remains unchanged (per-user path is simply unavailable).

## Validation Checklist

- Multi-user room: verify each speaker gets attributed partial/final events.
- Overlap speech: verify simultaneous streams and independent transcripts.
- Under provider slowdown/backpressure: verify AudioBridge participant/decode threads do not block.
- Queue saturation: verify stale PCM may be dropped but `stopped-talking` still results in stream close.
- Unload/reload: verify clean stream teardown and re-init.
- Regression: verify existing mixed-audio path still functions.

## Future Extensions

- Add additional providers by implementing `abmod_provider_*` adapters.
- Add VAD/stream timeout policies to close silent streams without waiting for `stopped-talking`.
- Add queue limits/metrics per user for observability.
