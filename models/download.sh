#!/usr/bin/env bash
# Locus - download recommended embedding + reranker models (multilingual).
#
#   bge-m3              Q8_0   ~635 MB   1024-dim, 8K ctx, multilingual + code
#   bge-reranker-v2-m3  Q8_0   ~636 MB   cross-encoder reranker, multilingual
#
# Total: ~1.27 GB. Both files land next to this script in models/.
# Already-downloaded files are skipped (size-checked).
#
# Bash equivalent of download.ps1 for macOS / Linux users without pwsh.
#
# Usage:
#   ./models/download.sh                # download both
#   ./models/download.sh --embedder     # embedder only
#   ./models/download.sh --reranker     # reranker only
#   ./models/download.sh --force        # re-download even if present
set -euo pipefail

WANT_EMBEDDER=0
WANT_RERANKER=0
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --embedder) WANT_EMBEDDER=1 ;;
        --reranker) WANT_RERANKER=1 ;;
        --force)    FORCE=1 ;;
        -h|--help)  grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
# Default: both, unless a specific one was requested.
if [ "$WANT_EMBEDDER" -eq 0 ] && [ "$WANT_RERANKER" -eq 0 ]; then
    WANT_EMBEDDER=1
    WANT_RERANKER=1
fi

MODELS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Portable file size in bytes (BSD stat on macOS, GNU stat on Linux).
file_size() {
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1" 2>/dev/null || echo 0
}

fmt_size() {
    awk -v b="$1" 'BEGIN{
        if (b>=1073741824) printf "%.2f GB", b/1073741824;
        else if (b>=1048576) printf "%.1f MB", b/1048576;
        else printf "%.0f KB", b/1024;
    }'
}

download_one() {
    local name="$1" url="$2" min_bytes="$3"
    local dest="$MODELS_DIR/$name"

    if [ -f "$dest" ] && [ "$FORCE" -eq 0 ]; then
        local size; size="$(file_size "$dest")"
        if [ "$size" -ge "$min_bytes" ]; then
            echo "[skip] $name already present ($(fmt_size "$size"))"
            return
        fi
        echo "[warn] $name exists but is too small ($(fmt_size "$size")) - re-downloading"
        rm -f "$dest"
    fi

    local tmp="$dest.partial"
    rm -f "$tmp"
    echo "[get ] $name"
    echo "       $url"
    curl -fL --retry 3 -o "$tmp" "$url"

    local size; size="$(file_size "$tmp")"
    if [ "$size" -lt "$min_bytes" ]; then
        rm -f "$tmp"
        echo "Downloaded file is suspiciously small ($(fmt_size "$size")) - Hugging Face may have returned an HTML error page. URL: $url" >&2
        exit 1
    fi
    mv -f "$tmp" "$dest"
    echo "[ok  ] $name ($(fmt_size "$size"))"
}

echo "Locus model downloader - destination: $MODELS_DIR"
echo

MIN_600MB=$((600 * 1024 * 1024))
if [ "$WANT_EMBEDDER" -eq 1 ]; then
    download_one "bge-m3-Q8_0.gguf" \
        "https://huggingface.co/ggml-org/bge-m3-Q8_0-GGUF/resolve/main/bge-m3-q8_0.gguf?download=true" \
        "$MIN_600MB"
fi
if [ "$WANT_RERANKER" -eq 1 ]; then
    download_one "bge-reranker-v2-m3-Q8_0.gguf" \
        "https://huggingface.co/gpustack/bge-reranker-v2-m3-GGUF/resolve/main/bge-reranker-v2-m3-Q8_0.gguf?download=true" \
        "$MIN_600MB"
fi

echo
echo "Done. Models are in $MODELS_DIR"
