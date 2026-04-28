#!/usr/bin/env bash
# Download ONNX gender model file for ABMod transcriber gender detection.
#
# Usage: download_gender_model.sh [DEST_DIR] [MODEL_URL]
#   DEST_DIR defaults to /usr/local/share/janus/models/gender
#   MODEL_URL defaults to ABMOD_GENDER_MODEL_URL env var, then a tested HF ONNX URL
#
# Output:
#   model.onnx

set -euo pipefail

DEST="${1:-/usr/local/share/janus/models/gender}"
DEFAULT_MODEL_URL="https://huggingface.co/prithivMLmods/Common-Voice-Gender-Detection-ONNX/resolve/main/onnx/model.onnx"
MODEL_URL="${2:-${ABMOD_GENDER_MODEL_URL:-$DEFAULT_MODEL_URL}}"

if [[ -z "$MODEL_URL" ]]; then
    echo "ERROR: missing gender model URL." >&2
    echo "Set ABMOD_GENDER_MODEL_URL or pass it as second argument." >&2
    exit 1
fi

mkdir -p "$DEST"
OUT="$DEST/model.onnx"

if [[ -f "$OUT" ]]; then
    echo "  [skip] model.onnx already present"
    exit 0
fi

echo "  [fetch] model.onnx"
if command -v curl &>/dev/null; then
    curl -fsSL --retry 3 -o "$OUT.tmp" "$MODEL_URL" && mv "$OUT.tmp" "$OUT"
elif command -v wget &>/dev/null; then
    wget -q --tries=3 -O "$OUT.tmp" "$MODEL_URL" && mv "$OUT.tmp" "$OUT"
else
    echo "ERROR: neither curl nor wget found" >&2
    exit 1
fi

echo "Done."
