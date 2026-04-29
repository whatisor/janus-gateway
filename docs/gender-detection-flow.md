# Gender Detection Flow (Transcript-Triggered)

## When gender detection runs

Gender detection is a one-shot inference per participant (`room_id|user_id`) and is triggered by transcript text.

High-level flow:

1. AudioBridge calls `abmod_on_participant_pcm(...)` for decoded per-user PCM.
2. The transcriber module enqueues this as `ABMOD_ITEM_PCM_USER`.
3. The worker thread dequeues the item and calls:
   - `abmod_gender_on_pcm(ctx->gender, room_id, user_id, pcm, samples, sampling_rate, channels)`
4. Gender module (`abmod_gender_on_pcm`) only buffers audio:
   - Converts PCM to mono float 16 kHz (`to_mono_16k`).
   - Stores rolling buffer per user in `GenderState::mono16k`.
   - Trims to `gender_max_buffer_ms`.
5. Provider transcript callback (`abmod_on_transcript`) triggers inference:
   - Calls `abmod_gender_trigger_on_transcript(...)`.
   - Trigger policy is fully owned by gender module config.
   - Trigger is gated by transcript quality:
     - `gender_trigger_final_only` (default `true`)
     - `gender_trigger_min_transcript_confidence` (default `0.50`)
     - `gender_trigger_allow_unknown_confidence` (default `false`)
     - `gender_trigger_min_chars` (default `4`)
   - Trigger is deferred until at least `gender_window_ms` audio is buffered.
   - ONNX inference uses only the latest fixed window (`gender_window_ms`), not the whole talking span.
   - ONNX inference runs exactly once for that user, then result is cached.

Result lifecycle:

- `pending`: still buffering or waiting transcript trigger.
- `ready`: inference succeeded and confidence passed threshold.
- `unavailable`: inference failed or low confidence after one attempt.
- `disabled`: gender engine disabled or model init failed.

## Why this mode helps with noise

- Inference is not attempted from PCM alone.
- Trigger depends on real transcript text from provider output.
- This reduces accidental inference on pure silence/noise segments.
