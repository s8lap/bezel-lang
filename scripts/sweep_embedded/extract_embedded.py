#!/usr/bin/env python3
"""Extract Bezel programs embedded in C++ test sources.

usage: python3 -I extract_embedded.py <repo-root> <outdir> [<git-ref>=8cb5f314 sources read via git show]

Writes <outdir>/<sanitized-file>__<line>[_n].bzl and <outdir>/index.tsv
(columns: name, file, line, test, kind, note) and <outdir>/stats.txt.

kinds: whole     literal group containing a `module` header
       composed  helper(RAW) call whose helper prepends a prefix constant defined in the file
       fragment  looks like Bezel (class/method/function...) but no `module` header; compiled alone
Unextractable sites (a Bezel-looking literal joined with + / non-literal operands) are counted in stats.
"""
import os, re, subprocess, sys

root, outdir = sys.argv[1], sys.argv[2]
ref = sys.argv[3] if len(sys.argv) > 3 else 'HEAD'
os.makedirs(outdir, exist_ok=True)

files = subprocess.check_output(['git', '-C', root, 'ls-tree', '-r', '--name-only', ref, 'tests'], text=True).split()
files = [f for f in files if re.search(r'\.(cpp|hpp)$', f)]

ESC = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '0': '\0', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}


def decode(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'x':
                m = re.match(r'[0-9a-fA-F]+', s[i + 2:])
                if m:
                    out.append(chr(int(m.group(0), 16) & 0xff)); i += 2 + len(m.group(0)); continue
            if n in '01234567':
                m = re.match(r'[0-7]{1,3}', s[i + 1:])
                out.append(chr(int(m.group(0), 8))); i += 1 + len(m.group(0)); continue
            out.append(ESC.get(n, n)); i += 2; continue
        out.append(c); i += 1
    return ''.join(out)


def lex(src):
    """yield (kind, text, start_offset, end_offset): kind in str, raw, other (single punct / ident run), skipped comments/ws not yielded as other but as 'ws'."""
    toks = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in ' \t\r\n':
            j = i
            while j < n and src[j] in ' \t\r\n': j += 1
            toks.append(('ws', src[i:j], i, j)); i = j; continue
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            toks.append(('ws', '', i, j)); i = j; continue
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            toks.append(('ws', '', i, j)); i = j; continue
        m = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\v\f\n]{0,16})\(').match(src, i)
        if m:
            delim = m.group(1)
            endtok = ')' + delim + '"'
            j = src.find(endtok, m.end())
            if j < 0:
                toks.append(('other', src[i], i, i + 1)); i += 1; continue
            toks.append(('raw', src[m.end():j], i, j + len(endtok))); i = j + len(endtok); continue
        m = re.compile(r'(?:u8|u|U|L)?"').match(src, i)
        if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == '_') or m.group(0) != '"'):
            j = m.end()
            while j < n and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            toks.append(('str', decode(src[m.end():j]), i, j + 1)); i = j + 1; continue
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            toks.append(('other', src[i:j + 1], i, j + 1)); i = j + 1; continue
        m = re.compile(r'[A-Za-z_0-9]+').match(src, i)
        if m:
            toks.append(('other', m.group(0), i, m.end())); i = m.end(); continue
        toks.append(('other', c, i, i + 1)); i += 1
    return toks


BEZ_HDR = re.compile(r'(?m)^\s*module\s+[A-Za-z_][\w.]*\s*;')
BEZ_LOOK = re.compile(r'\b(class|interface|method|function|choice|record|struct|property|end\s+\w+;)\b')
TEST_RE = re.compile(r'\b(?:BEZEL_TEST|TEST|TEST_F)\s*\(\s*([A-Za-z_0-9]+)\s*,\s*([A-Za-z_0-9]+)')

index = []
stats = dict(files=0, whole=0, composed=0, fragment=0, unextract_concat=0, unextract_dynamic=0)
unext_sites = []


def sanitize(f):
    return re.sub(r'[^A-Za-z0-9]+', '_', f[:-4] if f.endswith(('.cpp', '.hpp')) else f).strip('_')


for f in files:
    try:
        src = subprocess.check_output(['git', '-C', root, 'show', ref + ':' + f]).decode('utf-8', 'surrogateescape')
    except subprocess.CalledProcessError:
        continue
    if 'R"' not in src and '"' not in src:
        continue
    stats['files'] += 1
    toks = lex(src)
    line_of = lambda off: src.count('\n', 0, off) + 1
    tests = [(m.start(), m.group(1) + '.' + m.group(2)) for m in TEST_RE.finditer(src)]

    def test_at(off):
        best = ''
        for p, nme in tests:
            if p <= off: best = nme
            else: break
        return best

    # group adjacent literals
    groups = []  # (start_idx, end_idx, text, parts)
    k = 0
    while k < len(toks):
        if toks[k][0] in ('str', 'raw'):
            j = k
            parts = [toks[k]]
            last = k
            j = k + 1
            while j < len(toks):
                if toks[j][0] == 'ws': j += 1; continue
                if toks[j][0] in ('str', 'raw'):
                    parts.append(toks[j]); last = j; j += 1; continue
                break
            groups.append((k, last, ''.join(p[1] for p in parts), parts))
            k = last + 1
        else:
            k += 1

    def prev_sig(idx):
        j = idx - 1
        while j >= 0 and toks[j][0] == 'ws': j -= 1
        return toks[j] if j >= 0 else None

    def next_sig(idx):
        j = idx + 1
        while j < len(toks) and toks[j][0] == 'ws': j += 1
        return toks[j] if j < len(toks) else None

    # prefix constants: IDENT = <group> ;   (raw literal assigned at definition)
    prefixes = {}
    for (a, b, text, parts) in groups:
        p = prev_sig(a)
        if p and p[1] == '=':
            # identifier before '='
            j = a - 1
            while j >= 0 and toks[j][0] == 'ws': j -= 1
            j -= 1
            while j >= 0 and toks[j][0] == 'ws': j -= 1
            if j >= 0 and toks[j][0] == 'other' and re.match(r'^[A-Za-z_]\w*$', toks[j][1]):
                nx = next_sig(b)
                if nx and nx[1] == ';' and (BEZ_HDR.search(text) or BEZ_LOOK.search(text)):
                    prefixes[toks[j][1]] = text
    # helpers: NAME ( ... ) { return [std::string(] K [)] + ARG ; }
    helpers = {}
    for m in re.finditer(r'(\w+)\s*\(\s*(?:const\s+)?(?:std::string|std::string_view|const\s+char\s*\*)\s*&?\s*(\w+)\s*\)\s*(?:->\s*std::string\s*)?\{\s*return\s+(?:std::string\(\s*)?(\w+)\s*\)?\s*\+\s*(?:std::string\(\s*)?(\w+)\s*\)?\s*;', src):
        if m.group(3) in prefixes and m.group(4) == m.group(2):
            helpers[m.group(1)] = m.group(3)
        elif m.group(4) in prefixes and m.group(3) == m.group(2):
            helpers[m.group(1)] = ('suffix', m.group(4))

    seen_lines = {}
    for gi, (a, b, text, parts) in enumerate(groups):
        start_off = toks[a][2]
        line = line_of(start_off)
        is_prefix_def = False
        p = prev_sig(a)
        # skip the prefix constants themselves? keep them as whole programs too (fine).
        hdr = BEZ_HDR.search(text)
        look = BEZ_LOOK.search(text) and ('end ' in text)
        if not (hdr or (look and len(text) > 20)):
            # Bezel-looking text in short literals is ignored
            continue
        # composition
        kind = 'whole' if hdr else 'fragment'
        note = ''
        full = text
        # helper( GROUP )
        j = a - 1
        while j >= 0 and toks[j][0] == 'ws': j -= 1
        if j >= 0 and toks[j][1] == '(':
            j -= 1
            while j >= 0 and toks[j][0] == 'ws': j -= 1
            if j >= 0 and toks[j][1] in helpers:
                h = helpers[toks[j][1]]
                if isinstance(h, tuple):
                    full = text + prefixes[h[1]]
                else:
                    full = prefixes[h] + text
                kind = 'composed'; note = 'helper=' + toks[j][1]
        # concatenation with + around the literal group
        pv = prev_sig(a); nx = next_sig(b)
        joined = (pv and pv[1] == '+') or (nx and nx[1] == '+')
        if joined and kind != 'composed':
            # try: IDENT + GROUP / GROUP + IDENT with IDENT a prefix constant (possibly via std::string(K))
            ok = False
            if pv and pv[1] == '+':
                jj = a - 1
                while jj >= 0 and toks[jj][0] == 'ws': jj -= 1
                jj -= 1
                while jj >= 0 and toks[jj][0] == 'ws': jj -= 1
                if jj >= 0 and toks[jj][1] == ')':
                    jj -= 1
                    while jj >= 0 and toks[jj][0] == 'ws': jj -= 1
                if jj >= 0 and toks[jj][1] in prefixes:
                    full = prefixes[toks[jj][1]] + text; kind = 'composed'; note = 'prefix+' ; ok = True
            if not ok:
                stats['unextract_concat'] += 1
                unext_sites.append((f, line, 'concat-with-+'))
                note = 'joined-with-+ (partial)'
                # still try the partial text as a fragment
        if hdr is None and kind == 'fragment' and joined and not note:
            note = 'joined'
        base = '%s__%d' % (sanitize(f), line)
        n = seen_lines.get(base, 0); seen_lines[base] = n + 1
        name = base if n == 0 else '%s_%d' % (base, n)
        with open(os.path.join(outdir, name + '.bzl'), 'w', encoding='utf-8', errors='surrogateescape') as fh:
            fh.write(full)
        stats['whole' if kind == 'whole' else ('composed' if kind == 'composed' else 'fragment')] += 1
        index.append((name, f, line, test_at(start_off), kind, note))

# dynamic sites: BEZEL-looking text passed through format/replace is not detectable; count 'module' in
# literals with printf-style %s/%d or {} placeholders
for (name, f, line, test, kind, note) in index:
    pass

with open(os.path.join(outdir, 'index.tsv'), 'w') as fh:
    fh.write('name\tfile\tline\ttest\tkind\tnote\n')
    for r in index:
        fh.write('\t'.join(str(x) for x in r) + '\n')
with open(os.path.join(outdir, 'stats.txt'), 'w') as fh:
    for k2, v in stats.items():
        fh.write('%s=%d\n' % (k2, v))
    fh.write('unextractable sites (partial text still extracted as fragment):\n')
    for s in unext_sites:
        fh.write('  %s:%d %s\n' % s)
print(stats)
