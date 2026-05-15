"""
Build-time source-encoding lint (S5.P).

Called from check_no_em_dashes.cmake via:
  python scripts/check_no_em_dashes.py <source_dir> <allowlist_path>

Exits 0 on clean, 1 on violations (CMake turns non-zero into a build error).
"""

import os
import sys

# Target characters (codepoint -> label).
# Use codepoint escapes so this script is itself pure ASCII.
TARGET = {
    chr(0x2014): 'em-dash (U+2014)',
    chr(0x2013): 'en-dash (U+2013)',
    chr(0x2192): 'right-arrow (U+2192)',
    chr(0x2190): 'left-arrow (U+2190)',
    chr(0x2026): 'ellipsis (U+2026)',
    chr(0x2018): 'left-single-quote (U+2018)',
    chr(0x2019): 'right-single-quote (U+2019)',
    chr(0x201C): 'left-double-quote (U+201C)',
    chr(0x201D): 'right-double-quote (U+201D)',
    chr(0x00A0): 'non-breaking-space (U+00A0)',
}

EXTENSIONS = {'.h', '.cpp'}


def load_allowlist(path):
    """Returns (bare_files, filecp_pairs) where filecp_pairs is {file: {U+XXXX}}."""
    bare_files = set()
    filecp = {}  # file -> set of 'U+XXXX' strings
    if not os.path.exists(path):
        return bare_files, filecp
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip().replace('\\', '/')
            if not line or line.startswith('#'):
                continue
            colon = line.rfind(':')
            if colon != -1 and colon + 2 < len(line) and line[colon+1:colon+3] == 'U+':
                fpath = line[:colon]
                cp_str = line[colon+1:]
                filecp.setdefault(fpath, set()).add(cp_str.upper())
            else:
                bare_files.add(line)
    return bare_files, filecp


def cp_str(ch):
    return 'U+{:04X}'.format(ord(ch))


def check_file(abs_path, rel_path, bare_files, filecp):
    """Returns list of violation strings, or empty list if clean."""
    rel_forward = rel_path.replace('\\', '/')
    if rel_forward in bare_files:
        return []

    per_file_exempt = filecp.get(rel_forward, set())

    try:
        content = open(abs_path, encoding='utf-8', errors='replace').read()
    except Exception as e:
        return [f'  {rel_forward} -- could not read: {e}']

    violations = []
    seen_chars = set()
    for ch, label in TARGET.items():
        if ch in content:
            cpstr = cp_str(ch)
            if cpstr in per_file_exempt:
                continue
            if cpstr not in seen_chars:
                seen_chars.add(cpstr)
                violations.append(f'  {rel_forward} -- {label}')
    return violations


def main():
    if len(sys.argv) < 3:
        print('Usage: check_no_em_dashes.py <source_dir> <allowlist_path>', file=sys.stderr)
        sys.exit(2)

    source_dir = sys.argv[1]
    allowlist_path = sys.argv[2]

    bare_files, filecp = load_allowlist(allowlist_path)

    scan_roots = [
        os.path.join(source_dir, 'src'),
        os.path.join(source_dir, 'tests'),
    ]

    violations = []
    for scan_root in scan_roots:
        if not os.path.isdir(scan_root):
            continue
        for dirpath, dirs, files in os.walk(scan_root):
            dirs[:] = sorted(d for d in dirs
                             if d not in {'build', '.git', '.locus', 'vcpkg',
                                          'models', 'node_modules'})
            for fname in sorted(files):
                ext = os.path.splitext(fname)[1].lower()
                if ext not in EXTENSIONS:
                    continue
                abs_path = os.path.join(dirpath, fname)
                rel_path = os.path.relpath(abs_path, source_dir).replace('\\', '/')
                v = check_file(abs_path, rel_path, bare_files, filecp)
                violations.extend(v)

    if violations:
        count = len(violations)
        msg = '\n'.join(violations)
        print(f'S5.P encoding lint: {count} source file(s) contain non-ASCII '
              f'characters not on the allow-list:')
        print(msg)
        print("See CLAUDE.md 'No em-dashes anywhere.' and "
              "scripts/non_ascii_allowlist.txt.")
        print("To fix: run 'python scripts/strip_em_dashes.py' then rebuild.")
        sys.exit(1)

    print(f'S5.P encoding lint: OK ({source_dir}/src + tests clean)')


if __name__ == '__main__':
    main()
