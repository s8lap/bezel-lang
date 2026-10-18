#!/usr/bin/env python3
"""build_book.py — build the Bezel documentation site (mdBook) from docs/.

The normative documents stay single Markdown files (every docs tool edits and
checks them in that form). This script splits each one into pages for mdBook:
one page per top-level (`##`) section, and for the changelog one page per ten
revisions. It rewrites every internal link so it resolves on the split site:

  - `#anchor` and `Bezel_*.md#anchor` links go to the page that holds the
    anchor — an explicit `<a id="…">` or a heading's GitHub-style slug (the
    slug is pinned with an explicit id where mdBook would derive another);
  - links to other repository files (proposals, research, tests, src) go to
    the file on GitHub (REPO_URL).

Usage:
  scripts/build_book.py              split into build/book/src and run `mdbook build`
                                     (site in build/book/html)
  scripts/build_book.py --check      split into a scratch dir and verify links only;
                                     no mdbook needed (run by scripts/check_docs.sh)
  scripts/build_book.py --out DIR    split into DIR (DIR/src, DIR/book.toml)
  scripts/build_book.py --no-mdbook  split only

--check fails when a link that resolves in the single-file document does not
resolve on the split site (the split itself broke it). Links already broken
in the source are counted and reported, not failed: the source-level anchor
check of check_docs.sh owns those.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True,
                      text=True).stdout.strip() or os.getcwd()
DOCS = os.path.join(ROOT, 'docs')
BOOK_CFG = os.path.join(DOCS, 'book')          # book.toml, theme/, intro pages
REPO_URL = 'https://github.com/s8lap/bezel-lang/blob/main/'

# (file, slug, title, how to split)
DOCUMENTS = [
    ('Bezel_Language_Spec.md', 'spec', 'Language Specification', 'h2'),
    ('Bezel_Standard_Library.md', 'stdlib', 'Standard Library', 'h2'),
    ('Bezel_Guide_01_Memory_and_Ownership.md', 'guide-01', 'Memory and Ownership', 'h2'),
    ('Bezel_Guide_02_Errors_and_Nullability.md', 'guide-02', 'Errors and Nullability', 'h2'),
    ('Bezel_Guide_03_Modeling_Data.md', 'guide-03', 'Modeling Data', 'h2'),
    ('Bezel_Guide_04_Concurrency.md', 'guide-04', 'Concurrency', 'h2'),
    ('Bezel_Guide_05_Collections_and_Iteration.md', 'guide-05', 'Collections and Iteration', 'h2'),
    ('Bezel_Guide_06_Contracts_and_Purity.md', 'guide-06', 'Contracts and Purity', 'h2'),
    ('Bezel_Guide_07_Style.md', 'guide-07', 'Style', 'h2'),
    ('Bezel_For_Code_Generation.md', 'writing', 'Writing Reference', 'h2'),
    ('Bezel_Design_Rationale.md', 'rationale', 'Design Rationale', 'h2'),
    ('Bezel_Changelog.md', 'changelog', 'Changelog', 'h3-decade'),
]
PARTS = [('Guides', ['guide-01', 'guide-02', 'guide-03', 'guide-04', 'guide-05', 'guide-06',
                     'guide-07']),
         ('Reference', ['spec', 'stdlib', 'writing']),
         ('Background', ['rationale', 'changelog'])]

FENCE = re.compile(r'^(```|~~~)')
HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
ANCHOR = re.compile(r'<a\s+id="([^"]+)"\s*>\s*</a>')
LINK = re.compile(r'(?<!!)(\[(?:[^\[\]]|\[[^\]]*\])*\]\()([^)\s]+)(\))')


def gh_slug(text):
    """GitHub's heading slug: lowercase; drop everything but letters, digits,
    spaces, hyphens and underscores; spaces become hyphens."""
    t = re.sub(r'<[^>]+>', '', text)
    t = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', t)
    t = t.replace('`', '').strip().lower()
    t = ''.join(ch for ch in t if ch.isalnum() or ch in ' -_')
    return t.replace(' ', '-')


def split_lines(text):
    """Yield (line, in_code) for each line."""
    in_code = False
    for line in text.split('\n'):
        if FENCE.match(line):
            yield line, True
            in_code = not in_code
            continue
        yield line, in_code


def page_slug(n, title):
    s = gh_slug(title)[:60].strip('-') or 'section'
    return f'{n:02d}-{s}'


def decade_of(title):
    m = re.search(r'v0\.(\d+)', title)
    return None if not m else int(m.group(1)) // 10


class Page:
    def __init__(self, doc, slug, title):
        self.doc, self.slug, self.title = doc, slug, title
        self.lines = []
        self.anchors = set()

    @property
    def path(self):
        return f'{self.doc}/{self.slug}.md'


def split_document(fname, dslug, dtitle, mode):
    text = open(os.path.join(DOCS, fname), encoding='utf-8').read()
    pages = []
    intro = Page(dslug, 'index', dtitle)
    cur = intro
    pages.append(cur)
    seen_gh = {}                      # GitHub de-duplicates slugs per document
    pending_ids = []                  # <a id> lines just above a heading
    decade = None
    n = 0
    for line, in_code in split_lines(text):
        h = None if in_code else HEADING.match(line)
        if not in_code and ANCHOR.fullmatch(line.strip() or ' '):
            pending_ids.append(line)
            continue
        if h:
            level, title = len(h.group(1)), h.group(2)
            start = False
            if mode == 'h2' and level == 2:
                start = True
                ptitle = re.sub(r'`', '', re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', title))
            elif mode == 'h3-decade' and level == 3:
                d = decade_of(title)
                if d is not None and d != decade:
                    decade = d
                    start = True
                    lo, hi = d * 10, d * 10 + 9
                    ptitle = f'v0.{lo}–v0.{hi}' if d else 'v0.1–v0.9'
            if start:
                n += 1
                cur = Page(dslug, page_slug(n, ptitle), ptitle)
                pages.append(cur)
                if mode == 'h3-decade':
                    cur.lines.append(f'# {ptitle}')
                    cur.lines.append('')
            base = gh_slug(title)
            k = seen_gh.get(base, 0)
            seen_gh[base] = k + 1
            gid = base if k == 0 else f'{base}-{k}'
            # shift so the page's top heading is level 1 (`##` in the spec,
            # `###` in a changelog page under its generated `#` title)
            newlevel = max(1, level - 1) if (mode == 'h2' or level >= 3) else level
            cur.lines.extend(pending_ids)
            explicit = set()
            for pl in pending_ids:
                explicit.update(ANCHOR.findall(pl))
            cur.anchors.update(explicit)
            pending_ids = []
            # pin the GitHub-style slug, which the documents' own links use;
            # mdBook derives heading ids per page and would number repeats
            # differently
            if gid not in explicit:
                cur.lines.append(f'<a id="{gid}"></a>')
            cur.anchors.add(gid)
            cur.lines.append('#' * newlevel + ' ' + title)
            continue
        if pending_ids:
            cur.lines.extend(pending_ids)
            for pl in pending_ids:
                cur.anchors.update(ANCHOR.findall(pl))
            pending_ids = []
        if not in_code:
            cur.anchors.update(ANCHOR.findall(line))
        cur.lines.append(line)
    cur.lines.extend(pending_ids)
    for pl in pending_ids:
        cur.anchors.update(ANCHOR.findall(pl))
    return pages


def build(out, check):
    pages_by_doc = {}
    for fname, dslug, dtitle, mode in DOCUMENTS:
        pages_by_doc[fname] = split_document(fname, dslug, dtitle, mode)
    # anchor -> page, per document
    where = {fname: {} for fname in pages_by_doc}
    for fname, pages in pages_by_doc.items():
        for p in pages:
            for a in p.anchors:
                where[fname].setdefault(a, p)
    doc_files = {f for f, *_ in DOCUMENTS}
    stats = {'links': 0, 'external': 0, 'repo': 0, 'broken_split': [], 'broken_source': 0}

    def rewrite(fname, page):
        out_lines = []
        for line, in_code in split_lines('\n'.join(page.lines)):
            if in_code:
                out_lines.append(line)
                continue

            def fix(m):
                tgt = m.group(2)
                if re.match(r'(https?:|mailto:)', tgt):
                    stats['external'] += 1
                    return m.group(0)
                stats['links'] += 1
                path, _, frag = tgt.partition('#')
                if not path:
                    target_doc = fname
                else:
                    norm = os.path.normpath(os.path.join('docs', path))
                    if norm.startswith('docs/') and os.path.basename(norm) in doc_files \
                            and os.path.dirname(norm) == 'docs':
                        target_doc = os.path.basename(norm)
                    else:
                        stats['repo'] += 1
                        url = REPO_URL + norm + ('#' + frag if frag else '')
                        return m.group(1) + url + m.group(3)
                if not frag:
                    dest = pages_by_doc[target_doc][0]
                    return m.group(1) + os.path.relpath(dest.path, page.doc) + m.group(3)
                dest = where[target_doc].get(frag)
                if dest is None:
                    stats['broken_source'] += 1
                    dest = pages_by_doc[target_doc][0]
                    return m.group(1) + os.path.relpath(dest.path, page.doc) + m.group(3)
                if dest is page:
                    return m.group(1) + '#' + frag + m.group(3)
                return m.group(1) + os.path.relpath(dest.path, page.doc) + '#' + frag + m.group(3)

            out_lines.append(LINK.sub(fix, line))
        return '\n'.join(out_lines).rstrip() + '\n'

    src = os.path.join(out, 'src')
    if os.path.exists(src):
        shutil.rmtree(src)
    os.makedirs(src)
    summary = ['# Summary', '', '[Introduction](README.md)', '']
    for part, slugs in PARTS:
        summary.append(f'# {part}')
        summary.append('')
        for fname, dslug, dtitle, mode in DOCUMENTS:
            if dslug not in slugs:
                continue
            pages = pages_by_doc[fname]
            os.makedirs(os.path.join(src, dslug), exist_ok=True)
            summary.append(f'- [{dtitle}]({pages[0].path})')
            for p in pages:
                body = rewrite(fname, p)
                if p.slug == 'index' and not body.lstrip().startswith('#'):
                    body = f'# {dtitle}\n\n' + body
                open(os.path.join(src, p.path), 'w', encoding='utf-8').write(body)
                if p.slug != 'index':
                    title = p.title.replace('[', '(').replace(']', ')')
                    summary.append(f'  - [{title}]({p.path})')
        summary.append('')
    open(os.path.join(src, 'SUMMARY.md'), 'w', encoding='utf-8').write('\n'.join(summary))
    shutil.copy(os.path.join(BOOK_CFG, 'README.md'), os.path.join(src, 'README.md'))
    for f in ('book.toml',):
        shutil.copy(os.path.join(BOOK_CFG, f), os.path.join(out, f))
    theme = os.path.join(out, 'theme')
    if os.path.exists(theme):
        shutil.rmtree(theme)
    shutil.copytree(os.path.join(BOOK_CFG, 'theme'), theme)

    # verify: every in-site link points at an existing page and anchor
    ids_by_page = {}
    for fname, pages in pages_by_doc.items():
        for p in pages:
            body = open(os.path.join(src, p.path), encoding='utf-8').read()
            ids = {a for line, in_code in split_lines(body) if not in_code
                   for a in ANCHOR.findall(line)}
            ids_by_page[p.path] = (body, ids)
    for path, (body, ids) in ids_by_page.items():
        for line, in_code in split_lines(body):
            if in_code:
                continue
            for m in LINK.finditer(line):
                tgt = m.group(2)
                if re.match(r'(https?:|mailto:)', tgt):
                    continue
                p, _, frag = tgt.partition('#')
                dest = os.path.normpath(os.path.join(os.path.dirname(path), p)) if p else path
                if dest not in ids_by_page:
                    stats['broken_split'].append(f'{path}: {tgt} (no such page)')
                elif frag and frag not in ids_by_page[dest][1]:
                    stats['broken_split'].append(f'{path}: {tgt} (no such anchor)')
    npages = sum(len(p) for p in pages_by_doc.values())
    biggest = max(os.path.getsize(os.path.join(src, p)) for p in ids_by_page)
    return stats, npages, biggest


def main():
    args = sys.argv[1:]
    check = '--check' in args
    out = None
    if '--out' in args:
        out = args[args.index('--out') + 1]
    tmp = None
    if out is None:
        if check:
            base = '/var/tmp/bezel-scratch' if os.path.isdir('/var/tmp') else None
            if base:
                os.makedirs(base, exist_ok=True)
            tmp = tempfile.mkdtemp(prefix='book.', dir=base)
            out = tmp
        else:
            out = os.path.join(ROOT, 'build', 'book')
    os.makedirs(out, exist_ok=True)
    stats, npages, biggest = build(out, check)
    bad = stats['broken_split']
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    if bad:
        print(f'BOOK: {len(bad)} link(s) the split broke (they resolve in the single-file document):')
        print('\n'.join('  ' + b for b in bad[:40]))
        sys.exit(1)
    summary = (f'{npages} pages, largest {biggest // 1024} KiB; {stats["links"]} internal links resolved, '
               f'{stats["repo"]} pointed at repository files, '
               f'{stats["broken_source"]} already unresolved in the source')
    if check:
        print(f'BOOK: clean ({summary})')
        return
    print(f'split: {summary}')
    if '--no-mdbook' in args:
        return
    if not shutil.which('mdbook'):
        sys.exit('build_book: mdbook not found (cargo install mdbook --version 0.4.52 --locked)')
    r = subprocess.run(['mdbook', 'build', out])
    if r.returncode:
        sys.exit(r.returncode)
    print(f'site: {os.path.join(out, "html")}')


if __name__ == '__main__':
    main()
