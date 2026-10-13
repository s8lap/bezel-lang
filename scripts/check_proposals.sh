#!/usr/bin/env bash
# check_proposals.sh — the one-command gate for the docs/proposals ledger.
#
# Symmetric with scripts/check_docs.sh (the normative-docs gate): a single
# blessed signal that the proposals folder and its README index agree. Runs
# three mechanical checks and exits non-zero if any fails.
#
#   1. Bijection  — every numbered proposal file (`NNNN-*.md`) has exactly one
#                   row in the README's status-sectioned tables, and every
#                   row's number links to an existing proposal file. One named
#                   non-proposal exceptions: `0046-audit-v0.199.md` and `0118-audit-v0.281.md` (the 0046/0118
#                   acceptance audit, a supporting record linked from proposal
#                   0046's own Status row — not itself a proposal).
#   2. Token      — every proposal's Status row leads with a canonical token
#                   from the closed set `under-review` / `accepted` /
#                   `graduated-v0.NNN` / `declined` / `study` (token, ` — `,
#                   then the prose record), and its README row's status cell
#                   leads with the SAME token.
#   3. Section    — each README row sits in the section its token belongs to:
#                   `under-review`/`study` in **Pending decision**, `accepted`
#                   in **Accepted, awaiting graduation or implementation**,
#                   `graduated-v0.NNN` in **Graduated**, `declined` in
#                   **Declined**.
#
# Usage:  scripts/check_proposals.sh
#
# This is a lint, not a proof: it catches the mechanical drift class that bites
# a two-carrier ledger (a proposal filed without a row, a status changed in one
# place, a row parked in the wrong section). It does NOT check that the prose
# after the token is truthful — that is the spec-change-process's review step.
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "check_proposals: not in a git repo" >&2; exit 2; }

python3 - <<'PY'
import re, sys, os

PROPDIR = 'docs/proposals'
README = os.path.join(PROPDIR, 'README.md')

# Named non-proposal exception(s): numbered files that are supporting records,
# not proposals (see the header comment).
EXCLUDE = {'0046-audit-v0.199.md', '0118-audit-v0.281.md'}

TOKEN_RE = re.compile(r'^(under-review|accepted|graduated-v0\.\d+|declined|study)(?: — |$)')
SEC_PEND = 'Pending decision'
SEC_ACC = 'Accepted, awaiting graduation or implementation'
SEC_GRAD = 'Graduated'
SEC_DECL = 'Declined'

def section_for(token):
    if token is None:
        return None
    if token.startswith('graduated-'):
        return SEC_GRAD
    if token == 'declined':
        return SEC_DECL
    if token == 'accepted':
        return SEC_ACC
    return SEC_PEND  # under-review / study

fail = 0

# ---- gather proposal files and their Status-row tokens ---------------------
files = sorted(f for f in os.listdir(PROPDIR)
               if re.match(r'\d{4}-.*\.md$', f) and f not in EXCLUDE)
file_token = {}    # filename -> token or None
file_norow = []
STATUS_TABLE = re.compile(r'^\| \*\*Status\*\* \| (.*?) \|\s*$')
STATUS_BOLD = re.compile(r'^\*\*Status:\*\* (.*?)\s*$')
for fn in files:
    tok = None
    row = None
    with open(os.path.join(PROPDIR, fn), encoding='utf-8') as f:
        for line in f:
            m = STATUS_TABLE.match(line) or STATUS_BOLD.match(line)
            if m:
                row = m.group(1)
                break
    if row is None:
        file_norow.append(fn)
    else:
        tm = TOKEN_RE.match(row)
        tok = tm.group(1) if tm else None
    file_token[fn] = tok

# ---- parse the README's status-sectioned tables ----------------------------
rows = []          # (num, target, status_cell, section, lineno)
section = None
try:
    readme_lines = open(README, encoding='utf-8').read().split('\n')
except FileNotFoundError:
    print(f"check_proposals: missing {README}", file=sys.stderr); sys.exit(2)
for i, line in enumerate(readme_lines, 1):
    h = re.match(r'^##\s+(.*?)\s*$', line)
    if h:
        section = h.group(1)
        continue
    if not (line.startswith('|') and line.rstrip().endswith('|')):
        continue
    cells = [c.strip() for c in line.rstrip()[1:-1].split(' | ')]
    if not cells or cells[0].startswith('-') or cells[0] in ('#', 'Record'):
        continue
    m = re.match(r'^\[(\d{4})\]\(([^)#\s]+)\)$', cells[0]) or re.match(r'^(\d{4})$', cells[0])
    if not m:
        continue
    num = m.group(1)
    target = m.group(2) if m.lastindex and m.lastindex >= 2 else None
    if target is None and len(cells) > 1:
        lm = re.search(r'\]\(([^)#\s]+)(?:#[^)\s]*)?\)', cells[1])
        target = lm.group(1) if lm else None
    status_cell = cells[2] if len(cells) > 2 else ''
    rows.append((num, target, status_cell, section, i))

# ---- 1. Bijection ----------------------------------------------------------
bad = []
by_num = {}
for num, target, status_cell, sec, ln in rows:
    by_num.setdefault(num, []).append((target, ln))
for num, hits in sorted(by_num.items()):
    if len(hits) > 1:
        bad.append(f"  {num}: {len(hits)} README rows (lines {', '.join(str(l) for _, l in hits)})")
row_targets = set()
for num, target, status_cell, sec, ln in rows:
    if target is None:
        bad.append(f"  {num} (README line {ln}): row has no link to a proposal file")
        continue
    base = os.path.basename(target)
    row_targets.add(base)
    if not os.path.exists(os.path.join(PROPDIR, target)):
        bad.append(f"  {num} (README line {ln}): links to missing file {target}")
    elif not base.startswith(num):
        bad.append(f"  {num} (README line {ln}): links to {base}, which is not a {num}-* file")
for fn in files:
    if fn not in row_targets:
        bad.append(f"  {fn}: no README row links to it")
if bad:
    fail = 1
    print(f"BIJECTION: {len(bad)} violation(s):")
    print("\n".join(bad))
else:
    print(f"BIJECTION: clean ({len(files)} proposal files ↔ {len(rows)} README rows; "
          f"{len(EXCLUDE)} named non-proposal exception)")

# ---- 2. Token --------------------------------------------------------------
bad = []
for fn in file_norow:
    bad.append(f"  {fn}: no Status row found (neither `| **Status** |` nor `**Status:**`)")
for fn in files:
    if fn in file_norow:
        continue
    if file_token[fn] is None:
        bad.append(f"  {fn}: Status row does not lead with a closed-set token")
readme_token = {}
for num, target, status_cell, sec, ln in rows:
    tm = TOKEN_RE.match(status_cell)
    rtok = tm.group(1) if tm else None
    readme_token[num] = rtok
    if rtok is None:
        bad.append(f"  {num} (README line {ln}): status cell does not lead with a closed-set token")
        continue
    if target:
        base = os.path.basename(target)
        ftok = file_token.get(base)
        if ftok is not None and ftok != rtok:
            bad.append(f"  {num}: README token '{rtok}' != file token '{ftok}' ({base})")
if bad:
    fail = 1
    print(f"TOKEN: {len(bad)} violation(s):")
    print("\n".join(bad))
else:
    print(f"TOKEN: clean (all {len(files)} Status rows and {len(rows)} README status cells "
          f"lead with an agreeing closed-set token)")

# ---- 3. Section ------------------------------------------------------------
bad = []
counts = {SEC_PEND: 0, SEC_ACC: 0, SEC_GRAD: 0, SEC_DECL: 0}
for num, target, status_cell, sec, ln in rows:
    tok = readme_token.get(num)
    want = section_for(tok)
    if want is None:
        continue  # already a TOKEN violation; don't double-report
    if sec != want:
        bad.append(f"  {num} (README line {ln}): token '{tok}' belongs in '{want}', row sits in '{sec}'")
    else:
        counts[want] += 1
if bad:
    fail = 1
    print(f"SECTION: {len(bad)} violation(s):")
    print("\n".join(bad))
else:
    print(f"SECTION: clean (pending {counts[SEC_PEND]} / accepted {counts[SEC_ACC]} / graduated {counts[SEC_GRAD]} / "
          f"declined {counts[SEC_DECL]})")


# ---- TABLE: Markdown table integrity in the ledger files ----------------------
# Added v0.267 after a composition gate found six blank lines inserted *inside*
# DECLINED.md's tables (a blank line terminates a GFM table, so ~80 rows rendered
# as literal pipe text) and one row pasted outside the Graduated ledger entirely.
# Neither mechanical gate could see either defect.
def _pipes(ln):
    # count unescaped pipes: a `\\|` inside a cell is literal text, not a separator.
    # (DECLINED.md's *Structural / anonymous unions* row is the standing example, and it
    # has defeated every naive counter written against this file.)
    return len(re.findall(r'(?<!\\)\|', ln))

tbad = []
for path in (README, os.path.join(PROPDIR, 'DECLINED.md')):
    lines = open(path, encoding="utf-8").read().split("\n")
    hdr_cells = None          # cell count of the table we are inside; None = not in a table
    prev_row = False
    for i, ln in enumerate(lines, 1):
        is_row = ln.startswith("|")
        is_delim = is_row and set(ln.replace("|", "").replace(" ", "")) <= {"-", ":"} and "-" in ln
        if is_delim:
            hdr_cells = _pipes(ln)
            prev_row = True
            continue
        if is_row:
            nxt = lines[i] if i < len(lines) else ""
            is_header = nxt.startswith("|") and set(nxt.replace("|", "").replace(" ", "")) <= {"-", ":"} and "-" in nxt
            if is_header:
                prev_row = True
                continue          # a header row legitimately precedes its delimiter
            if hdr_cells is None:
                tbad.append(f"  {os.path.basename(path)}:{i}: table row outside any table (no delimiter above)")
            elif _pipes(ln) != hdr_cells:
                tbad.append(f"  {os.path.basename(path)}:{i}: row has {_pipes(ln)} pipes, table header has {hdr_cells}")
            prev_row = True
            continue
        if not ln.strip():
            # a blank line ends the table; flag it only when a row follows immediately
            if prev_row and i < len(lines) and lines[i].startswith("|"):
                tbad.append(f"  {os.path.basename(path)}:{i}: blank line between two table rows (terminates the table)")
            hdr_cells = None
        prev_row = False
if tbad:
    fail = 1
    print(f"TABLE: {len(tbad)} violation(s):")
    print("\n".join(tbad))
else:
    print("TABLE: clean (ledger tables well-formed; no interior blank lines, no orphan rows)")

sys.exit(1 if fail else 0)
PY
RC=$?

[ "$RC" -eq 0 ] && echo "check_proposals: OK" || echo "check_proposals: FAIL"
exit "$RC"
