# ABMod Gender ONNX

`libabmod_gender_onnx` is an AudioBridge AB module that performs per-user gender classification from short PCM windows.

## Build

- Configure Janus with ONNX Runtime available:
  - `./configure --with-onnxruntime=/usr/local`
- Build/install normally:
  - `make -j`
  - `make install`

If ONNX Runtime is not enabled, a stub module is built and `abmod_create` returns `NULL`.

## Runtime load

Load at room runtime with AudioBridge `configure`:

```json
{
  "request": "configure",
  "room": 1234,
  "abmod_load": "/usr/local/lib/janus/abmodules/libabmod_gender_onnx.so",
  "abmod_config": "{\"model_path\":\"/usr/share/janus/models/gender/model.onnx\",\"window_ms\":3000,\"min_confidence\":0.60,\"emit_interval_ms\":1500}"
}
```

## `abmod_config` keys

- `model_path` (string): ONNX model file path.
- `window_ms` (int): audio window size for inference, default `3000`.
- `min_confidence` (float in `[0,1]`): minimum accepted confidence, default `0.60`.
- `emit_interval_ms` (int): per-user minimum emit interval, default `1500`.
- `max_buffer_ms` (int): maximum retained per-user buffer, default `8000`.

## Emitted events

The module emits `event="gender"` via the existing AB module event envelope. Payload includes:

- `room_id`
- `user_id`
- `label` (`male` or `female`)
- `confidence`
- `male_prob`
- `female_prob`
- `window_ms`
- `sample_rate`
- `model_path`
- `ts_us`

## Model contract

Current implementation expects a model with one waveform input (`input_values`, `audio`, `input`, or first input) and one logits output with two classes (`female`, `male` convention).
