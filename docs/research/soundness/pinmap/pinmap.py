#!/usr/bin/env python3
"""pinmap.py — which kernel clauses and §28/§30.5 blocks does anything pin?

Build-free, read-only. Reads the spec, the rule corpus (tests/conformance/rules),
the latest run of each completeness matrix (docs/research/*-matrix/runs), and the
gtest/lit sources under tests/, and reports, per kernel clause (K1.1 … K9.5, A1 …
A15) and per §28 / §30.5 block:

  * how many spec blocks carry a `derives:` / `axiom` tag naming it;
  * how many matrix cells cite it (by K/A id or by section);
  * how many rule-corpus cases pin it — directly (the case's `rule:` quote holds
    verbatim inside a block that derives from the clause) or by section only;
  * how many gtest / lit sources name the section.

The report ends with the clauses and blocks nothing pins, and with the quote-drift
figure (rule quotes that no longer hold verbatim in the current documents).

Usage:  python3 -I docs/research/soundness/pinmap/pinmap.py [--repo PATH] [--out DIR]
Output: <out>/pinmap.md, <out>/pinmap.json
"""
import argparse, collections, glob, json, os, re, sys

# ----------------------------------------------------------------------------- helpers

def norm(s):
    """The quote normalization the v0.336–v0.340 trains used (rulequotes.py)."""
    s = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', s)
    s = (s.replace('`', '').replace('*', '').replace('—', '--').replace('–', '-')
          .replace('’', "'").replace('\\|', '|'))
    return re.sub(r'\s+', ' ', s).strip()

K_ID = re.compile(r'\bK([1-9])(?:\.(\d{1,2}))?\b')
A_ID = re.compile(r'\bA(\d{1,2})\b')
SEC = re.compile(r'§\s*(\d{1,2}(?:\.\d{1,2}){0,2})')

# ----------------------------------------------------------------------------- spec

def load_spec(repo):
    path = os.path.join(repo, 'docs', 'Bezel_Language_Spec.md')
    lines = open(path, encoding='utf-8').read().split('\n')
    m = re.search(r'\*\*Version (0\.\d+)', '\n'.join(lines[:10]))
    return lines, (m.group(1) if m else '?')

def chapter_ranges(lines):
    """Line ranges of chapter 28 and of §30.5 (the kernel's two homes)."""
    def find(pat):
        for i, l in enumerate(lines):
            if re.match(pat, l):
                return i
        raise SystemExit(f'heading not found: {pat}')
    s28, e28 = find(r'^## 28\. '), find(r'^## 29\. ')
    s305, e305 = find(r'^### 30\.5 '), find(r'^### 30\.6 ')
    return [(s28, e28), (s305, e305)]

def sec_key(section):
    """Cases, cells and tests cite §28.3, never §28.3.8: fold the h4 numbers."""
    return '28.3' if section.startswith('28.3.') else section

def split_blocks(lines, ranges):
    """Blocks are h3/h4 headings and bold-head paragraphs (`**Title** …`) — the
    granularity the 0046 audit used. Each block records its section number (from
    the enclosing heading), its title, its text, and the K/A ids its derivation
    tags name."""
    blocks = []
    for (s, e) in ranges:
        section, cur = None, None
        for i in range(s, e):
            l = lines[i]
            h = re.match(r'^(#{3,4}) (\d{1,2}(?:\.\d{1,2}){1,2}) (.*)$', l)
            if h:
                section = h.group(2)
                cur = dict(section=section, title=h.group(3).strip(), kind='heading',
                           line=i + 1, text=[])
                blocks.append(cur)
                continue
            b = re.match(r'^\*\*([^*]{3,160}?)\*\*', l)
            if b and section:
                cur = dict(section=section, title=b.group(1).strip(' .'), kind='bold',
                           line=i + 1, text=[])
                blocks.append(cur)
            if cur is not None:
                cur['text'].append(l)
    for b in blocks:
        raw = '\n'.join(b['text'])
        b['text'] = raw
        b['norm'] = norm(raw)
        # derivation / axiom tags: the parenthetical the 0046 trains introduced
        tags = set()
        # tags are read off the link-stripped text: `[K2.1](#sec-kernel-k2)` → `K2.1`
        for m in re.finditer(r'derives:\s*([^)]*)', b['norm']):
            tags.update(f'K{a}' + (f'.{n}' if n else '') for a, n in K_ID.findall(m.group(1)))
            tags.update(f'A{n}' for n in A_ID.findall(m.group(1)))
        for m in re.finditer(r'axiom\s+A(\d{1,2})', b['norm']):
            tags.add(f'A{m.group(1)}')
        b['tags'] = sorted(tags)
        # every K/A id the block mentions at all (weaker evidence)
        b['mentions'] = sorted({f'K{a}' + (f'.{n}' if n else '') for a, n in K_ID.findall(raw)}
                               | {f'A{n}' for n in A_ID.findall(raw)})
    return blocks

def kernel_clauses(lines):
    """K1.1 … K9.5 from §28.3.1 (numbered items under each **Kn —** head) and the
    axioms A1 … A15 from the register table."""
    s = next(i for i, l in enumerate(lines) if l.startswith('#### 28.3.1 '))
    e = next(i for i, l in enumerate(lines) if l.startswith('#### 28.3.3 '))
    clauses, k = {}, None
    for i in range(s, e):
        l = lines[i]
        h = re.match(r'^\*\*K([1-9]) — ([^.]*)\.', l)
        if h:
            k = f'K{h.group(1)}'
            clauses[k] = dict(title=h.group(2), items={}, line=i + 1)
            continue
        if k and re.match(r'^\s*\d{1,2}\. ', l):
            n = re.match(r'^\s*(\d{1,2})\. ', l).group(1)
            clauses[k]['items'][f'{k}.{n}'] = norm(l)
        t = re.match(r'^\| K(3\.\d{1,2}) \|', l)        # the consume table's rows
        if t and 'K3' in clauses:
            clauses['K3']['items'].setdefault('K' + t.group(1), norm(l))
        a = re.match(r'^\| (A\d{1,2}) \| (.*?) \|', l)
        if a:
            clauses.setdefault('A', dict(title='axiom register', items={}, line=i + 1))
            clauses['A']['items'][a.group(1)] = norm(l)
    flat = {}
    for k, v in clauses.items():
        if k != 'A':
            flat[k] = v['title']
        flat.update(v['items'])
    return flat  # id -> full normalized clause text (title for K1..K9 heads)

# ----------------------------------------------------------------------------- corpus

def load_corpus(repo):
    root = os.path.join(repo, 'tests', 'conformance', 'rules')
    sys.path.insert(0, root)
    import corpus  # the corpus's own header parser
    out = []
    for cid, path, deps in corpus.case_entries(os.path.join(root, 'cases')):
        h = corpus.parse_header(open(path, encoding='utf-8').read(), path)
        out.append(dict(id=cid, section=h.get('section', ''), doc=h.get('doc', ''),
                        expect=h.get('expect', ''), rule=h.get('rule', ''),
                        cites=h.get('cites', ''), issue=h.get('issue', '')))
    base = {}
    bpath = os.path.join(root, 'baseline.tsv')
    if os.path.exists(bpath):
        for l in open(bpath, encoding='utf-8'):
            f = l.rstrip('\n').split('\t')
            if len(f) >= 2 and not f[0].startswith('case'):
                base[f[0]] = f[1]
    return out, base

def quote_segments(rule):
    r = norm(rule)
    return [s.strip(' .,;:"') for s in re.split(r'…|\.\.\.', r) if len(s.strip(' .,;:"')) >= 25]

# ----------------------------------------------------------------------------- matrices

def load_matrices(repo):
    cells = []
    for d in sorted(glob.glob(os.path.join(repo, 'docs', 'research', '*-matrix'))):
        runs = sorted(glob.glob(os.path.join(d, 'runs', 'v0.*.json')),
                      key=lambda p: int(re.search(r'v0\.(\d+)', p).group(1)))
        if not runs:
            continue
        data = json.load(open(runs[-1], encoding='utf-8'))
        name = os.path.basename(d).replace('-matrix', '')
        for c in data.get('cells', []) + data.get('routes', []):
            txt = ' '.join(str(c.get(k, '')) for k in ('cite', 'rule', 'quote'))
            ids = {f'K{a}' + (f'.{n}' if n else '') for a, n in K_ID.findall(txt)}
            ids |= {f'A{n}' for n in A_ID.findall(c.get('cite', ''))}
            secs = set(SEC.findall(txt))
            cells.append(dict(matrix=name, run=os.path.basename(runs[-1]), row=c.get('row'),
                              col=c.get('col'), route=c.get('route'), status=c.get('status'),
                              verdict=c.get('verdict'), ids=sorted(ids), secs=sorted(secs),
                              rule=c.get('rule', '')))
    return cells

# ----------------------------------------------------------------------------- gtest / lit

def load_tests(repo):
    """Per-section citation counts in gtest (.cpp) and lit (.bzl) sources: a file
    counts once per section it names, by `spec_28_3`-style test names or `§28.3`
    in comments. Coarse by construction — these tests are pinned to issues."""
    per_sec = collections.Counter()
    files = 0
    for p in glob.glob(os.path.join(repo, 'tests', '**', '*.cpp'), recursive=True) + \
             glob.glob(os.path.join(repo, 'tests', 'lit', '**', '*.bzl'), recursive=True):
        if '/conformance/' in p:
            continue
        try:
            t = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        files += 1
        secs = set()
        for m in re.finditer(r'spec_(28|30)_(\d{1,2})(?:_(\d{1,2}))?', t):
            s = f'{m.group(1)}.{m.group(2)}' + (f'.{m.group(3)}' if m.group(3) else '')
            secs.add(s)
        for m in re.finditer(r'§\s*(28(?:\.\d{1,2}){0,2}|30\.5)', t):
            secs.add(m.group(1))
        for s in secs:
            per_sec[s] += 1
    return per_sec, files

# ----------------------------------------------------------------------------- the join

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.getcwd())
    ap.add_argument('--out', default=os.path.join('scratch', 'research-soundness', 'pinmap', 'out'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    lines, rev = load_spec(a.repo)
    blocks = split_blocks(lines, chapter_ranges(lines))
    clauses = kernel_clauses(lines)
    cases, baseline = load_corpus(a.repo)
    cells = load_matrices(a.repo)
    test_secs, test_files = load_tests(a.repo)

    specN = norm('\n'.join(lines))
    stdlibN = norm(open(os.path.join(a.repo, 'docs', 'Bezel_Standard_Library.md'), encoding='utf-8').read())

    # --- corpus cases → blocks (verbatim-quote match) and → sections
    in_scope = [c for c in cases if c['doc'] == 'spec' and
                (c['section'].startswith('28') or c['section'].startswith('30.5'))]
    drift = dict(total=0, hold=0, paraphrase=0, short=0)
    for c in in_scope:
        segs = quote_segments(c['rule'])
        c['blocks'] = []
        if not segs:
            drift['short'] += 1
            c['quote'] = 'short'
            continue
        drift['total'] += 1
        if all(s in specN for s in segs):
            drift['hold'] += 1
            c['quote'] = 'verbatim'
            for b in blocks:
                if all(s in b['norm'] for s in segs):
                    c['blocks'].append(b)
        else:
            drift['paraphrase'] += 1
            c['quote'] = 'paraphrase'
    # whole-corpus drift, for the headline figure
    all_drift = dict(total=0, hold=0)
    for c in cases:
        segs = quote_segments(c['rule'])
        D = specN if c['doc'] == 'spec' else stdlibN if c['doc'] == 'stdlib' else None
        if not segs or D is None:
            continue
        all_drift['total'] += 1
        all_drift['hold'] += all(s in D for s in segs)

    # --- per clause
    per_clause = {}
    for cid in clauses:
        tagged = [b for b in blocks if cid in b['tags'] or (cid in b['mentions'] and b['kind'] == 'heading' and False)]
        mentioned = [b for b in blocks if cid in b['mentions']]
        ctext = clauses[cid]
        direct = [c for c in in_scope if any(cid in b['tags'] for b in c['blocks'])
                  or (c.get('quote') == 'verbatim' and len(ctext) > 60
                      and all(seg in ctext for seg in quote_segments(c['rule'])))]
        # by-section: the case's section is the section of a block tagged with cid
        secs = {sec_key(b['section']) for b in tagged}
        bysec = [c for c in in_scope if c['section'] in secs and c not in direct]
        mcells = [m for m in cells if cid in m['ids']]
        per_clause[cid] = dict(title=clauses[cid], tagged_blocks=len(tagged), mentioning_blocks=len(mentioned),
                               corpus_direct=len(direct), corpus_by_section=len(bysec),
                               corpus_direct_fail=sum(1 for c in direct if baseline.get(c['id']) == 'fail'),
                               matrix_cells=len(mcells),
                               matrix_open=sum(1 for m in mcells if m['status'] not in ('decided', 'n/a')))

    # --- per block
    per_block = []
    for b in blocks:
        direct = [c for c in in_scope if b in c['blocks']]
        bysec = [c for c in in_scope if c['section'] == sec_key(b['section'])]
        mcells = [m for m in cells if sec_key(b['section']) in m['secs']]
        per_block.append(dict(section=b['section'], title=b['title'][:70], kind=b['kind'], line=b['line'],
                              tags=b['tags'], words=len(b['norm'].split()),
                              corpus_direct=len(direct), corpus_section=len(bysec),
                              matrix_cells_section=len(mcells),
                              tests_section=test_secs.get(sec_key(b['section']), 0)))

    # --- report
    R = []
    w = R.append
    w(f'# Pin map — spec v{rev}\n')
    w('Build-free join of the spec\'s §28 / §30.5 blocks, their kernel derivation tags, the rule corpus, '
      'the latest matrix runs and the gtest/lit sources. Numbers are counts of *things that would notice* '
      'if the clause or block changed meaning; "direct" means the case\'s own `rule:` quote holds verbatim '
      'inside a block that derives from the clause, "by section" means the case merely cites the section.\n')
    w(f'- Blocks parsed in §28 and §30.5: **{len(blocks)}** ({sum(1 for b in blocks if b["tags"])} carry a `derives:`/`axiom` tag)')
    w(f'- Kernel clauses and axioms indexed: **{len(clauses)}**')
    w(f'- Rule-corpus cases in §28/§30.5: **{len(in_scope)}** of {len(cases)}; matrix cells+routes (latest runs): **{len(cells)}**; test sources scanned: {test_files}')
    w(f'- Quote drift, §28/§30.5 cases: {drift["hold"]} of {drift["total"]} `rule:` quotes hold verbatim '
      f'({drift["paraphrase"]} paraphrase, {drift["short"]} too short to test). Whole corpus: {all_drift["hold"]} of {all_drift["total"]}.\n')

    w('## A. Kernel clauses and axioms: what pins each\n')
    w('| clause | tagged blocks | mentioning blocks | corpus direct (banked fail) | corpus by section | matrix cells (open) |')
    w('|---|---|---|---|---|---|')
    def key(c):
        m = re.match(r'([KA])(\d+)(?:\.(\d+))?', c)
        return (m.group(1), int(m.group(2)), int(m.group(3) or 0))
    for cid in sorted(per_clause, key=key):
        p = per_clause[cid]
        title = re.sub(r'^\| K[0-9.]+ \| ', '', p['title'])[:60]
        w(f'| **{cid}** {title} | {p["tagged_blocks"]} | {p["mentioning_blocks"]} | '
          f'{p["corpus_direct"]} ({p["corpus_direct_fail"]}) | {p["corpus_by_section"]} | {p["matrix_cells"]} ({p["matrix_open"]}) |')
    unpinned = [c for c, p in per_clause.items() if p['corpus_direct'] == 0 and p['matrix_cells'] == 0]
    weak = [c for c, p in per_clause.items() if p['corpus_direct'] == 0 and p['matrix_cells'] > 0]
    w(f'\n**Clauses with no direct corpus case and no matrix cell:** {", ".join(sorted(unpinned, key=key)) or "none"}')
    w(f'\n**Clauses pinned only by matrix cells (no direct corpus case):** {", ".join(sorted(weak, key=key)) or "none"}\n')

    w('## B. Blocks of §28 / §30.5 with nothing pinning them directly\n')
    w('A block counts as unpinned when no corpus case quotes it verbatim; "section" columns show the coarser evidence that exists.\n')
    w('| § | block | kind | tags | words | corpus direct | corpus §-only | matrix cells § | tests § |')
    w('|---|---|---|---|---|---|---|---|---|')
    for p in per_block:
        if p['corpus_direct'] == 0 and p['words'] >= 40:
            w(f'| {p["section"]} | {p["title"]} (l.{p["line"]}) | {p["kind"]} | {", ".join(p["tags"]) or "—"} | {p["words"]} | 0 | {p["corpus_section"]} | {p["matrix_cells_section"]} | {p["tests_section"]} |')
    n_unp = sum(1 for p in per_block if p['corpus_direct'] == 0 and p['words'] >= 40)
    w(f'\n{n_unp} of {sum(1 for p in per_block if p["words"] >= 40)} substantive blocks (≥40 words) have no directly-quoting corpus case.\n')

    w('## C. Per-section totals\n')
    w('| § | blocks | corpus cases (fail-banked) | verbatim quotes | matrix cells citing | gtest/lit sources citing |')
    w('|---|---|---|---|---|---|')
    secs = sorted({sec_key(b['section']) for b in blocks}, key=lambda s: [int(x) for x in s.split('.')])
    for s in secs:
        cs = [c for c in in_scope if c['section'] == s]
        w(f'| {s} | {sum(1 for b in blocks if sec_key(b["section"]) == s)} | {len(cs)} ({sum(1 for c in cs if baseline.get(c["id"]) == "fail")}) | '
          f'{sum(1 for c in cs if c.get("quote") == "verbatim")} | {sum(1 for m in cells if s in m["secs"])} | {test_secs.get(s, 0)} |')

    open(os.path.join(a.out, 'pinmap.md'), 'w', encoding='utf-8').write('\n'.join(R) + '\n')
    json.dump(dict(spec=rev, clauses=per_clause, blocks=per_block, drift=drift, all_drift=all_drift),
              open(os.path.join(a.out, 'pinmap.json'), 'w', encoding='utf-8'), indent=1)
    print('\n'.join(R[:12]))
    print(f'... written to {a.out}/pinmap.md')

if __name__ == '__main__':
    main()
