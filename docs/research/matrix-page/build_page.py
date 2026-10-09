#!/usr/bin/env python3
"""Build a spec-matrix page from a matrix directory.

Usage: python3 docs/research/matrix-page/build_page.py MATRIX_DIR OUT.html

MATRIX_DIR holds matrix.json (title, lede, legend, rows, cols) and runs/v*.json
(one file per run). The page is a generated view; the run files are the record.
Write OUT.html under ./scratch/ (gitignored), never into the tree.
"""
import html, json, pathlib, sys

here = pathlib.Path(__file__).resolve().parent
mdir = pathlib.Path(sys.argv[1]).resolve()
matrix = json.loads((mdir / 'matrix.json').read_text())
runs = {f.stem: json.loads(f.read_text()) for f in sorted((mdir / 'runs').glob('v*.json'))}
data = json.dumps({'matrix': matrix, 'runs': runs}, ensure_ascii=False).replace('</', '<\\/')
page = (here / 'page.template.html').read_text()
page = page.replace('__TITLE__', html.escape(matrix['title'])).replace('__DATA__', data)
pathlib.Path(sys.argv[2]).write_text(page)
print(f"{sys.argv[2]}: {matrix['title']}, {len(runs)} runs ({', '.join(runs)})")
