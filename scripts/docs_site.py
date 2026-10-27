#!/usr/bin/env python3
"""Stage README.md, docs/ and examples/ Markdown as MkDocs sources.

The repository keeps documentation next to the code it describes, which MkDocs
cannot use directly. This copies tracked Markdown pages and images into
.site-src/ with the same layout (README.md becomes the home page), and turns
links to anything that is not part of the site (sources, configs, validation
records) into links to the file on GitHub. Then run `mkdocs build`.
"""
import argparse
import posixpath
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSET_SUFFIXES = {'.png', '.jpg', '.jpeg', '.gif', '.svg'}
# Static pages copied under another name: a directory's README.md becomes its index.html.
RENAMED = {'examples/resource-dashboard/index.html': 'examples/resource-dashboard/prototype.html'}
LINK = re.compile(r'(!?\[[^\]]*\]\()([^)\s]+)(\))')


def tracked():
    out = subprocess.run(['git', 'ls-files'], cwd=ROOT, check=True, capture_output=True, text=True)
    return out.stdout.split()


def site_path(path):
    if path == 'README.md':
        return 'index.md'
    return RENAMED.get(path, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / '.site-src')
    parser.add_argument('--repo', default='https://github.com/anjn/amd-video-analysis-framework')
    parser.add_argument('--ref', default='main')
    args = parser.parse_args()

    files = tracked()
    pages = [f for f in files if f.endswith('.md') and
             (f == 'README.md' or f.startswith(('docs/', 'examples/')))]
    assets = [f for f in files if f.startswith(('docs/', 'examples/')) and
              (Path(f).suffix.lower() in ASSET_SUFFIXES or f in RENAMED)]
    staged = set(pages) | set(assets)

    def rewrite(source, match):
        prefix, target, suffix = match.groups()
        path, hash_, anchor = target.partition('#')
        if not path or re.match(r'^[a-z][a-z0-9+.-]*:', path) or path.startswith('/'):
            return match.group(0)
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
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
