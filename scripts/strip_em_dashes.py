"""
One-shot cleanup: replace non-ASCII typographic characters with ASCII equivalents.

Usage: python scripts/strip_em_dashes.py [--dry-run]

Characters replaced:
  U+2014 em dash         -> --
  U+2013 en dash         -> -
  U+2192 right arrow     -> ->
  U+2190 left arrow      -> <-
  U+2026 ellipsis        -> ...
  U+2018 left single q.  -> '
  U+2019 right single q. -> '
  U+201C left double q.  -> "
  U+201D right double q. -> "
  U+00A0 non-break space -> (space)

Allow-list (lines with these chars are preserved verbatim):
  src/frontends/gui/chat_panel.cpp  -- plan-step status glyphs rendered in WebView2
"""

import os
import sys

REPLACEMENTS = [
    ('--', '--'),   # em dash
    ('-', '-'),    # en dash
    ('->', '->'),   # right arrow
    ('<-', '<-'),   # left arrow
    ('...', '...'),  # ellipsis
    (''', "'"),    # left single quote
    (''', "'"),    # right single quote
    ('"', '"'),    # left double quote
    ('"', '"'),    # right double quote
    (' ', ' '),    # non-breaking space
]

# Characters to preserve on a per-file basis (file -> set of codepoints to skip).
# Lines containing ANY of these chars in the listed file are written verbatim.
PRESERVE = {
    'src/frontends/gui/chat_panel.cpp': {
        '✓',  # CHECK MARK
        '✗',  # BALLOT X
        '⧗',  # BLACK HOURGLASS
        '○',  # WHITE CIRCLE
    }
}

SCAN_DIRS = ['src', 'tests', 'architecture', 'roadmap', 'scripts']
ROOT_MD_GLOB = ['*.md']
EXTENSIONS = {'.cpp', '.h', '.md', '.cmake', '.txt', '.ps1', '.py', '.json'}
SKIP_DIRS = {'build', '.locus', '.git', 'vcpkg', 'models', 'queries',
             'tree-sitter', 'node_modules'}

dry_run = '--dry-run' in sys.argv

def normalize_path(p):
    return p.replace('\\', '/')

def should_preserve_line(relpath, line):
    key = normalize_path(relpath)
    if key not in PRESERVE:
        return False
    preserve_chars = PRESERVE[key]
    return any(ch in line for ch in preserve_chars)

def process_file(filepath, relpath):
    try:
        original = open(filepath, encoding='utf-8', errors='replace').read()
    except Exception as e:
        print(f'  SKIP (read error): {relpath}: {e}')
        return False

    lines = original.splitlines(keepends=True)
    new_lines = []
    changed = False
    for line in lines:
        if should_preserve_line(relpath, line):
            new_lines.append(line)
            continue
        new_line = line
        for src, dst in REPLACEMENTS:
            new_line = new_line.replace(src, dst)
        if new_line != line:
            changed = True
        new_lines.append(new_line)

    if not changed:
        return False

    if dry_run:
        print(f'  would rewrite: {relpath}')
        return True

    try:
        open(filepath, 'w', encoding='utf-8', newline='').write(''.join(new_lines))
        print(f'  rewritten: {relpath}')
        return True
    except Exception as e:
        print(f'  ERROR writing {relpath}: {e}')
        return False

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    total_changed = 0

    # Scan configured directories
    for scan_dir in SCAN_DIRS:
        abs_dir = os.path.join(root, scan_dir)
        if not os.path.isdir(abs_dir):
            continue
        for dirpath, dirs, files in os.walk(abs_dir):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for fname in sorted(files):
                ext = os.path.splitext(fname)[1].lower()
                if ext not in EXTENSIONS:
                    continue
                fp = os.path.join(dirpath, fname)
                rp = os.path.relpath(fp, root).replace('\\', '/')
                if process_file(fp, rp):
                    total_changed += 1

    # Root-level .md files
    for fname in sorted(os.listdir(root)):
        if fname.lower().endswith('.md'):
            fp = os.path.join(root, fname)
            rp = fname
            if process_file(fp, rp):
                total_changed += 1

    action = 'would rewrite' if dry_run else 'rewrote'
    print(f'\nDone: {action} {total_changed} file(s).')

if __name__ == '__main__':
    main()
