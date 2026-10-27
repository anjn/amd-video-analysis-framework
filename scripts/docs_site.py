#!/usr/bin/env python3
"""Stage the user guide (docs/guide/) and developer information (docs/dev/) as MkDocs sources.

The guide becomes the site root and the developer information /dev/. Pages and
images are copied into .site-src/, and links to anything that is not part of
the site (README, sources, configs, validation records) become links to the
file on GitHub. Then run `mkdocs build`.

Pages come in pairs, `x.md` (English) and `x.ja.md` (Japanese). Pages link to
the counterparts in their own language, which works on GitHub; on the site the
i18n plugin resolves plain `.md` links per language. The page that was
translated starts with `<!-- source: <path> <git blob hash> -->` naming the
original it follows: the developer information is written in English, the
guide and the READMEs in Japanese. --check-translations lists translations
whose original changed since, and --stamp FILE records the current original
after a translation was updated.
"""
import argparse
import os
import posixpath
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE_DIRS = {'docs/guide/': '', 'docs/dev/': 'dev/'}
ASSET_SUFFIXES = {'.png', '.jpg', '.jpeg', '.gif', '.svg'}
# Static pages copied under another name: a directory's README.md becomes its index.html.
RENAMED = {'examples/resource-dashboard/index.html': 'examples/resource-dashboard/prototype.html'}
LINK = re.compile(r'(!?\[[^\]]*\]\()([^)\s]+)(\))')
STAMP = re.compile(r'^<!-- source: (\S+) ([0-9a-f]{40}) -->\n')


def tracked():
    out = subprocess.run(['git', 'ls-files'], cwd=ROOT, check=True, capture_output=True, text=True)
    return out.stdout.split()


def site_path(path):
    for prefix, target in SITE_DIRS.items():
        if path.startswith(prefix):
            return target + path[len(prefix):]
    return RENAMED.get(path, path)


def counterpart(path):
    return path[:-len('.ja.md')] + '.md' if path.endswith('.ja.md') else path[:-3] + '.ja.md'


def blob(path):
    return subprocess.run(['git', 'hash-object', path], cwd=ROOT, check=True,
                          capture_output=True, text=True).stdout.strip()


def check_translations(files):
    """Print translations whose original changed or that lack a stamp; return their count."""
    present = set(files)
    stale = 0
    for path in sorted(f for f in files if f.endswith('.ja.md') and counterpart(f) in present):
        pair = [path, counterpart(path)]
        stamped = [(p, m) for p in pair if (m := STAMP.match((ROOT / p).read_text()))]
        if len(stamped) != 1:
            problem, where = 'exactly one page of the pair needs a source stamp', path
        else:
            where, match = stamped[0]
            if match.group(1) != counterpart(where):
                problem = f'stamp names {match.group(1)}, expected {counterpart(where)}'
            elif match.group(2) != blob(match.group(1)):
                problem = f'{match.group(1)} changed since this translation'
            else:
                continue
        stale += 1
        print(f'::warning file={where}::{problem}' if os.environ.get('GITHUB_ACTIONS')
              else f'{where}: {problem}')
    return stale


def stamp(path):
    text = (ROOT / path).read_text()
    original = counterpart(path)
    line = f'<!-- source: {original} {blob(original)} -->\n'
    (ROOT / path).write_text(line + STAMP.sub('', text, count=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.site-src')
    parser.add_argument('--repo', default='https://github.com/anjn/amd-video-analysis-framework')
    parser.add_argument('--ref', default='main')
    parser.add_argument('--check-translations', action='store_true',
                        help='list stale translations instead of staging')
    parser.add_argument('--stamp', nargs='+', metavar='FILE',
                        help='record the current original of updated translations')
    args = parser.parse_args()

    files = tracked()
    if args.stamp:
        for path in args.stamp:
            stamp(Path(path).resolve().relative_to(ROOT).as_posix())
        return
    if args.check_translations:
        stale = check_translations(files)
        print(f'{stale} stale translation(s)')
        return
    pages = [f for f in files if f.endswith('.md') and f.startswith(tuple(SITE_DIRS))]
    assets = [f for f in files if f.startswith(('docs/', 'examples/')) and
              (Path(f).suffix.lower() in ASSET_SUFFIXES or f in RENAMED)]
    staged = set(pages) | set(assets)

    def rewrite(source, match):
        prefix, target, suffix = match.groups()
        path, hash_, anchor = target.partition('#')
        if not path or re.match(r'^[a-z][a-z0-9+.-]*:', path) or path.startswith('/'):
            return match.group(0)
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
        if resolved.endswith('.ja.md'):
            resolved = counterpart(resolved)  # The i18n plugin picks the page language.
        if resolved in staged or (resolved + '/README.md') in staged:
            if resolved not in staged:
                resolved += '/README.md'
            new = posixpath.relpath(site_path(resolved), posixpath.dirname(site_path(source)) or '.')
        elif (ROOT / resolved).exists():
            kind = 'tree' if (ROOT / resolved).is_dir() else 'blob'
            new = f'{args.repo}/{kind}/{args.ref}/{resolved}'
        else:
            return match.group(0)  # Left for `mkdocs build --strict` to report.
        return f'{prefix}{new}{hash_}{anchor}{suffix}'

    if args.output.exists():
        shutil.rmtree(args.output)
    for path in pages:
        text = LINK.sub(lambda m, p=path: rewrite(p, m), (ROOT / path).read_text())
        target = args.output / site_path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    for path in assets:
        target = args.output / site_path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / path, target)
    print(f'{len(pages)} pages and {len(assets)} assets staged in {args.output}')


if __name__ == '__main__':
    main()
