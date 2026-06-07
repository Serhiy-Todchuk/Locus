#!/usr/bin/env bash
# Locus - download SMALL English-only embedding + reranker models.
#
#   bge-small-en-v1.5      Q8_0     ~37 MB    384-dim, 512 ctx, English
#   ms-marco-MiniLM-L6-v2  Q4_K_M   ~21 MB    22M-param cross-encoder, English.
#                                             ~13x faster than bge-reranker-v2-m3
#                                             (~8ms/rerank Release CPU vs ~100ms),
#                                             at the cost of English-only + weaker
#                                             long-passage handling.
#
# Total: ~58 MB. Use this profile when disk/RAM is tight or your workspace is
# English-only. Recall on non-English text and long chunks will be lower than
# the recommended bge-m3 set (see download.sh).
#
# Bash equivalent of download-small.ps1 for macOS / Linux users without pwsh.
#
# Usage:
#   ./models/download-small.sh                # download both
#   ./models/download-small.sh --embedder     # embedder only
#   ./models/download-small.sh --no-reranker  # skip reranker
#   ./models/download-small.sh --force        # re-download
set -euo pipefail

WANT_EMBEDDER=1
WANT_RERANKER=1
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --embedder)    WANT_EMBEDDER=1 ;;
        --no-reranker) WANT_RERANKER=0 ;;
        --force)       FORCE=1 ;;
        -h|--help)     grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

MODELS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

echo "Locus model downloader (small English profile) - destination: $MODELS_DIR"
echo

if [ "$WANT_EMBEDDER" -eq 1 ]; then
    download_one "bge-small-en-v1.5-Q8_0.gguf" \
        "https://huggingface.co/ggml-org/bge-small-en-v1.5-Q8_0-GGUF/resolve/main/bge-small-en-v1.5-q8_0.gguf?download=true" \
        $((30 * 1024 * 1024))
fi
if [ "$WANT_RERANKER" -eq 1 ]; then
    download_one "ms-marco-MiniLM-L6-v2-Q4_K_M.gguf" \
        "https://huggingface.co/sinjab/ms-marco-MiniLM-L6-v2-Q4_K_M-GGUF/resolve/main/ms-marco-MiniLM-L6-v2-Q4_K_M.gguf?download=true" \
        $((15 * 1024 * 1024))
fi

echo
echo "Done. Models are in $MODELS_DIR"
