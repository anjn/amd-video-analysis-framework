#!/usr/bin/env python3
"""Stage README.md, docs/ and examples/ Markdown as MkDocs sources.

The repository keeps documentation next to the code it describes, which MkDocs
cannot use directly. This copies tracked Markdown pages and images into
.site-src/ with the same layout (README.md becomes the home page), and turns
links to anything that is not part of the site (sources, configs, validation
records) into links to the file on GitHub. Then run `mkdocs build`.

Japanese translations sit next to their sources as `*.ja.md` and link to each
other with `.ja.md` names, which work on GitHub; on the site the i18n plugin
resolves plain `.md` links per language. Each translation starts with
`<!-- source: <path> <git blob hash> -->` recording the source it was made
from: --check-translations lists translations whose source changed since, and
--stamp FILE records the current source after a translation was updated.
"""
import argparse
import os
import posixpath
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSET_SUFFIXES = {'.png', '.jpg', '.jpeg', '.gif', '.svg'}
# Static pages copied under another name: a directory's README.md becomes its index.html.
# README language links for GitHub; the site has a language switcher instead.
README_LANGUAGE_LINK = re.compile(r'^\[(English|日本語)\]\(README(\.ja)?\.md\)\n\n?', re.M)
RENAMED = {'examples/resource-dashboard/index.html': 'examples/resource-dashboard/prototype.html'}
LINK = re.compile(r'(!?\[[^\]]*\]\()([^)\s]+)(\))')
STAMP = re.compile(r'^<!-- source: (\S+) ([0-9a-f]{40}) -->\n')


def tracked():
    out = subprocess.run(['git', 'ls-files'], cwd=ROOT, check=True, capture_output=True, text=True)
    return out.stdout.split()


def site_path(path):
    if path in ('README.md', 'README.ja.md'):
        return path.replace('README', 'index')
    return RENAMED.get(path, path)


def source_of(translation):
    return translation[:-len('.ja.md')] + '.md'


def blob(path):
    return subprocess.run(['git', 'hash-object', path], cwd=ROOT, check=True,
                          capture_output=True, text=True).stdout.strip()


def check_translations(files):
    """Print translations whose recorded source differs; return their count."""
    stale = 0
    for path in sorted(f for f in files if f.endswith('.ja.md')):
        match = STAMP.match((ROOT / path).read_text())
        source = source_of(path)
        if not match or match.group(1) != source:
            problem = f'missing or wrong source stamp (expected {source})'
        elif match.group(2) != blob(source):
            problem = f'{source} changed since this translation'
        else:
            continue
        stale += 1
        print(f'::warning file={path}::{problem}' if os.environ.get('GITHUB_ACTIONS')
              else f'{path}: {problem}')
    return stale


def stamp(path):
    text = (ROOT / path).read_text()
    source = source_of(path)
    line = f'<!-- source: {source} {blob(source)} -->\n'
    (ROOT / path).write_text(line + STAMP.sub('', text, count=1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.site-src')
    parser.add_argument('--repo', default='https://github.com/anjn/amd-video-analysis-framework')
    parser.add_argument('--ref', default='main')
    parser.add_argument('--check-translations', action='store_true',
                        help='list stale translations instead of staging')
    parser.add_argument('--stamp', nargs='+', metavar='FILE.ja.md',
                        help='record the current source of updated translations')
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
    pages = [f for f in files if f.endswith('.md') and
             (f in ('README.md', 'README.ja.md') or f.startswith(('docs/', 'examples/')))]
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
            resolved = source_of(resolved)  # The i18n plugin picks the page language.
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
        text = (ROOT / path).read_text()
        if path in ('README.md', 'README.ja.md'):
            text = README_LANGUAGE_LINK.sub('', text)
        text = LINK.sub(lambda m, p=path: rewrite(p, m), text)
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
