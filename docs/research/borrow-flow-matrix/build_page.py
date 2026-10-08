#!/usr/bin/env python3
"""Build the borrow-flow matrix page from runs/*.json.

Usage: python3 docs/research/borrow-flow-matrix/build_page.py OUT.html

The page is a generated view; the run files are the record. Write OUT.html
under ./scratch/ (gitignored), never into the tree.
"""
import json, pathlib, sys

here = pathlib.Path(__file__).resolve().parent
runs = {f.stem: json.loads(f.read_text()) for f in sorted((here / 'runs').glob('v*.json'))}
data = json.dumps({'runs': runs}, ensure_ascii=False).replace('</', '<\\/')
page = (here / 'page.template.html').read_text().replace('__DATA__', data)
pathlib.Path(sys.argv[1]).write_text(page)
print(f"{sys.argv[1]}: {len(runs)} runs ({', '.join(runs)})")
