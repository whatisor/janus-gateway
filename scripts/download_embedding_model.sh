#!/usr/bin/env bash
# Download all-MiniLM-L6-v2 ONNX model files for the abmod embedding guard.
#
# Usage: download_embedding_model.sh [DEST_DIR]
#   DEST_DIR defaults to /usr/local/share/janus/models/all-MiniLM-L6-v2
#
# Files downloaded:
#   model.onnx  — ONNX export of sentence-transformers/all-MiniLM-L6-v2
#   vocab.txt   — WordPiece vocabulary (standard BERT)

set -euo pipefail

HF_REPO="sentence-transformers/all-MiniLM-L6-v2"
HF_BASE="https://huggingface.co/${HF_REPO}/resolve/main"
DEST="${1:-/usr/local/share/janus/models/all-MiniLM-L6-v2}"

declare -A FILES=(
    ["model.onnx"]="${HF_BASE}/onnx/model.onnx"
    ["vocab.txt"]="${HF_BASE}/vocab.txt"
)

mkdir -p "$DEST"

download() {
    local name="$1" url="$2" dest="$DEST/$name"
    if [[ -f "$dest" ]]; then
        echo "  [skip] $name already present"
        return
    fi
    echo "  [fetch] $name"
    if command -v curl &>/dev/null; then
        curl -fsSL --retry 3 -o "$dest.tmp" "$url" && mv "$dest.tmp" "$dest"
    elif command -v wget &>/dev/null; then
        wget -q --tries=3 -O "$dest.tmp" "$url" && mv "$dest.tmp" "$dest"
    else
        echo "ERROR: neither curl nor wget found" >&2
        exit 1
    fi
}

echo "Fetching all-MiniLM-L6-v2 model files into: $DEST"
for name in "${!FILES[@]}"; do
    download "$name" "${FILES[$name]}"
done
echo "Done."
