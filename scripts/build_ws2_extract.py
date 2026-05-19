"""
Extract HTML articles from a Kiwix .zim file into a flat folder.

Called by scripts/build_ws2.ps1. Standalone-runnable too:

    python scripts/build_ws2_extract.py <zim_path> <out_dir> [--max N]

Outputs <out_dir>/<title>.html for the first N articles iterated from the
ZIM (cap N to keep the demo workspace small; pass --max 0 to extract all).
Skips redirects, non-text entries, and entries that aren't in the
articles namespace.

The official `zimdump` tool from openzim/zim-tools is the canonical
extractor but ships only for Linux/macOS. python-libzim has Windows
wheels and exposes the same data, so we use it directly.

Requires: pip install libzim
"""

import argparse
import re
import sys
from pathlib import Path

try:
    from libzim.reader import Archive
except ImportError:
    print("[error] python-libzim is not installed. Install with:", file=sys.stderr)
    print("  pip install libzim", file=sys.stderr)
    sys.exit(2)


_SLUG_RX = re.compile(r"[^A-Za-z0-9._-]+")


def safe_filename(title: str, fallback_idx: int) -> str:
    """Map an article title to a filesystem-safe basename.

    Wikipedia article paths often have characters Windows hates (':', '/',
    '*', '?'). Replace runs of unsafe chars with underscores; truncate to
    150 chars so paths stay reasonable. Empty / dot-only titles fall back
    to an index-based name."""
    s = _SLUG_RX.sub("_", title.strip())
    s = s.strip("._")
    if not s:
        s = f"article_{fallback_idx}"
    return s[:150]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("zim", help="Path to .zim archive")
    ap.add_argument("out_dir", help="Output directory for *.html articles")
    ap.add_argument("--max", type=int, default=5000,
                    help="Cap on extracted articles (0 = all). Default 5000.")
    ap.add_argument("--include", action="append", default=[],
                    help="Article path that MUST be extracted (use multiple "
                         "--include flags). Resolves redirects, counts toward "
                         "--max. Falls back to entry-id iteration for the rest.")
    args = ap.parse_args()

    zim_path = Path(args.zim)
    out_dir = Path(args.out_dir)
    if not zim_path.is_file():
        print(f"[error] ZIM not found: {zim_path}", file=sys.stderr)
        return 2

    out_dir.mkdir(parents=True, exist_ok=True)

    archive = Archive(zim_path)
    total_entries = archive.entry_count
    print(f"[info ] ZIM    : {zim_path.name}")
    print(f"[info ] entries: {total_entries}")
    print(f"[info ] out    : {out_dir}")
    print(f"[info ] cap    : {args.max if args.max > 0 else 'unlimited'}")

    extracted = 0
    skipped_redirect = 0
    skipped_nontext = 0
    seen_paths: set[str] = set()       # entry.path values we've already written
    seen_basenames: set[str] = set()    # filesystem-side dedupe

    def emit(entry, idx: int) -> bool:
        """Write one entry. Returns True if a file was written (or False
        for redirects / non-html / write failures)."""
        nonlocal extracted, skipped_redirect, skipped_nontext

        if entry.is_redirect:
            skipped_redirect += 1
            return False
        item = entry.get_item()
        mime = (item.mimetype or "").lower()
        if "html" not in mime:
            skipped_nontext += 1
            return False

        if entry.path in seen_paths:
            return False           # already emitted via --include or iteration
        seen_paths.add(entry.path)

        title = entry.title or entry.path or f"article_{idx}"
        base = safe_filename(title, idx)
        candidate = base
        dedupe = 1
        while candidate in seen_basenames:
            dedupe += 1
            candidate = f"{base}_{dedupe}"
        seen_basenames.add(candidate)

        out_path = out_dir / f"{candidate}.html"
        try:
            out_path.write_bytes(bytes(item.content))
        except Exception as e:
            print(f"[warn ] failed to write {out_path.name}: {e}", file=sys.stderr)
            return False

        extracted += 1
        if extracted % 500 == 0:
            print(f"[ok   ] extracted {extracted}", flush=True)
        return True

    # Pass 1: curated paths from --include. Resolve each by path. Unlike
    # the bulk iteration pass, redirect entries here resolve to their
    # target -- the user said "include HTTP" meaning "include the article
    # HTTP routes to" (Wikipedia uses redirect pages liberally for
    # acronym + alias coverage).
    if args.include:
        print(f"[info ] forcing {len(args.include)} curated article(s)")
    for raw in args.include:
        path = raw.strip()
        if not path:
            continue
        try:
            entry = archive.get_entry_by_path(path)
        except Exception:
            print(f"[warn ] curated path not in ZIM: {path}", file=sys.stderr)
            continue
        if entry.is_redirect:
            try:
                target = entry.get_redirect_entry()
                print(f"[info ] curated {path!r} -> {target.path!r} (redirect)")
                entry = target
            except Exception:
                print(f"[warn ] curated redirect resolve failed: {path}",
                      file=sys.stderr)
                continue
        emit(entry, -1)
        if args.max > 0 and extracted >= args.max:
            break

    # Pass 2: iterate by entry id to fill the rest.
    for i in range(total_entries):
        if args.max > 0 and extracted >= args.max:
            break
        try:
            entry = archive._get_entry_by_id(i)
        except Exception:
            continue
        emit(entry, i)

    print(f"[done ] extracted={extracted}  redirects_skipped={skipped_redirect}"
          f"  non-html_skipped={skipped_nontext}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
