#!/usr/bin/env python3
"""bzdiff — the model-vs-compiler differential harness (0047 §5.3; soundness README §3.2).

For every model program (bzcore L1, docs/research/soundness/bzcore-l1, read-only import) it:

  level 1  compares the model's three-valued verdict (accept / accept-with-warnings /
           reject:<rule>) with bezelc's (exit status + diagnostics mapped to rule families by
           families.py), at rule-family granularity;
  level 2  for programs both accept, runs the checked build once per model path (the path's
           choice vector becomes the command-line digit string) and compares the N/D/U event
           trace and the outcome (ok | FAULT=DisposedAccessException exit 70 | uncaught R |
           RACE = a use of a sent object | crash), then runs the unchecked build on the same
           paths and records crashes and silent divergences as supplementary evidence.

Usage:
    python3 -I bzdiff.py selftest [--out DIR]
    python3 -I bzdiff.py sweep --features F --vars N --len N [--if] [--try] [--while] [--body N]
                        [--sample N] [--seed S] [--no-fmove] [--out DIR] [--jobs 3]
    python3 -I bzdiff.py one "new a / disp a / use a"          # print, compile, run one program
Environment: BEZELC (path to bezelc), BZCORE (path to the model directory).
"""
import argparse, json, os, random, re, shutil, subprocess, sys, time, hashlib
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
BZCORE_DIR = os.environ.get('BZCORE', os.path.join(HERE, 'model'))   # the knobbed verbatim copy of docs/research/soundness/bzcore-l1/bzcore.py
sys.path.insert(0, HERE); sys.path.insert(0, BZCORE_DIR)
import bzcore                                   # noqa: E402  (read-only import of the model)
import bzlprint, families                       # noqa: E402

BEZELC = os.environ.get('BEZELC', '/tmp/claude-0/-home-user-bezel-lang/3f619ddf-d843-5ab0-a977-e5b3105857a0/'
                        'scratchpad/rel35/bezelc-0.0.0-f6863e62-linux-x86_64/bin/bezelc')

# ----------------------------------------------------------------------------- the model side

class TracedRun(bzcore.Run):
    """The model's machine with `use` events (a member access through a binding, or on a
    temporary) and the termination disposal of an uncaught exception split off."""
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.uncaught = False; self.term_events = []; self.term_start = None

    def step(self, s, site, declared):
        k = s[0]
        if k == 'send':
            loc = self.binding(s[1]); self.events.append(('send', loc)); self.foreign(loc); self.env[s[1]] = (loc, 'moved'); return
        if k == 'tsend':
            t = self.alloc(bzcore.TYPES_R); self.events.append(('send', t)); self.foreign(t); return
        if k == 'use':
            loc = self.read(s[1]); self.events.append(('use', loc)); return
        if k == 'tuse':
            t = self.alloc(bzcore.TYPES_R); self.events.append(('use', t)); self.dispose(t, 'boundary'); return
        return super().step(s, site, declared)

    def execute(self, prog):
        try:
            self.body(prog, ())
        except bzcore.Thrown as t:
            self.uncaught = True
            n = len(self.events)
            try:
                self.dispose(t.loc, 'termination')
            except bzcore.Halt as h:
                self.outcome = h.outcome; self.info = h.info
            self.term_events = self.events[n:]; self.term_start = n     # kept in events (the T1 oracle counts them); excluded from the compared trace
        except bzcore.Halt as h:
            self.outcome = h.outcome; self.info = h.info
        return self

bzcore.Run = TracedRun


def model_trace(r):
    evs = r.events if r.term_start is None else r.events[:r.term_start]
    return [(e[0], e[1]) for e in evs if e[0] in ('new', 'disp', 'use')]


def model_sends(r):
    """The ids sent, in send order (the k-th `S` the compiler prints is the k-th of these)."""
    evs = r.events if r.term_start is None else r.events[:r.term_start]
    return [e[1] for e in evs if e[0] == 'send']


def model_eval(prog):
    v, w, ctx = bzcore.check(prog)
    out = dict(verdict=v, warns=list(w))
    if v == 'accept':
        runs = bzcore.run_all(prog, ctx)
        out['runs'] = [dict(choices=''.join(str(c) for c in r.choices), trace=model_trace(r), outcome=r.outcome,
                            uncaught=r.uncaught, term=[(e[0], e[1]) for e in r.term_events if e[0] == 'disp'],
                            sent=model_sends(r))
                       for r in runs]
        out['bad'] = sorted(set(bzcore.judge(runs)))
    return out


def model_families(m):
    if m['verdict'] != 'accept':
        rule = m['verdict'][len('reject:'):]
        return 'reject', families.MODEL_FAMILY.get(rule, 'other'), []
    fams = sorted({families.MODEL_FAMILY.get(w, w) for w in m['warns']})
    return ('warn' if fams else 'accept'), None, fams

# ----------------------------------------------------------------------------- the compiler side

DIAG_RE = re.compile(r'^(?P<file>[^:\s]+\.bzl):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning: )?(?P<msg>.*)$')


def parse_diags(text):
    errs, warns, notes = [], [], []
    for line in text.splitlines():
        m = DIAG_RE.match(line)
        if not m:
            continue
        d = dict(line=int(m['line']), msg=m['msg'])
        if m['sev']:
            warns.append(d)
        elif families.NOTE_RE.search(m['msg']):
            notes.append(d)
        else:
            errs.append(d)
    return errs, warns, notes


def compile_one(src_path, out_path, mode):
    cp = subprocess.run([BEZELC, f'--{mode}-disposal', '-o', out_path, src_path],
                        capture_output=True, text=True, timeout=120)
    errs, warns, notes = parse_diags(cp.stdout + cp.stderr)
    return cp.returncode, errs, warns, notes, (cp.stdout + cp.stderr)


def compiler_verdict(rc, errs, warns):
    """(kind, reject family (first error), warning families, raw messages, all reject families)."""
    if rc != 0:
        if not errs:
            return 'reject', 'UNMAPPED:no-diagnostic(rc=%d)' % rc, [], [], []
        fams = [families.compiler_family(e['msg']) for e in errs]
        return 'reject', fams[0], [], [e['msg'] for e in errs], sorted(set(fams))
    kept = [w for w in warns if w['line'] >= bzlprint.BODY_FIRST_LINE and not families.ignored_warning(w['msg'])]
    fams = sorted({families.compiler_family(w['msg']) for w in kept})
    return ('warn' if fams else 'accept'), None, fams, [w['msg'] for w in kept], []


EV_RE = re.compile(r'^(N|D|U|S):(\d+)$')


def run_binary(path, digits, sent=()):
    """`sent`: the ids the model's path transferred to the foreign sink, in send order; the k-th
    bare `S` line marks the k-th of them as sent, and a later U of a sent id is a RACE."""
    args = [path] + ([digits] if digits else [])
    try:
        cp = subprocess.run(args, capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return dict(outcome='timeout', trace=[], rc=None, stderr='')
    trace, ended = [], False
    sent_so_far = set(); sends = list(sent); race = False
    for line in cp.stdout.splitlines():
        if line == 'END':
            ended = True; continue
        if line == 'S':
            if sends: sent_so_far.add(sends.pop(0))
            continue
        m = EV_RE.match(line)
        if not m:
            continue
        kind, i = m.group(1), int(m.group(2))
        if kind == 'U' and i in sent_so_far:
            race = True
        trace.append(({'N': 'new', 'D': 'disp', 'U': 'use'}[kind], i))
    err = cp.stderr
    if cp.returncode < 0:
        outcome = f'crash(sig{-cp.returncode})'
    elif cp.returncode == 0 and ended:
        outcome = 'ok'
    elif cp.returncode == 70 and 'DisposedAccessException' in err:
        outcome = 'FAULT'
    elif cp.returncode == 70 and 'Probe.R' in err:
        outcome = 'uncaught'
    elif cp.returncode == 70:
        outcome = 'exception:' + (err.strip().splitlines()[-1][:80] if err.strip() else '?')
    else:
        outcome = f'exit{cp.returncode}'
    if race and outcome in ('ok', 'uncaught'):
        outcome = 'RACE'
    return dict(outcome=outcome, trace=trace, rc=cp.returncode, stderr=err[-400:])


def compare_run(mr, cr):
    """Level-2 verdict for one path: 'agree' or a reason."""
    mo = mr['outcome']; co = cr['outcome']
    mt = mr['trace']; ct = cr['trace']
    if mo == 'ok':
        exp = 'uncaught' if mr['uncaught'] else 'ok'
        if co != exp:
            return f'outcome model={exp} compiler={co}'
        if ct != mt:
            return 'trace differs'
        return 'agree'
    if mo == 'FAULT':
        if co != 'FAULT':
            return f'outcome model=FAULT compiler={co}'
        if ct[:len(mt)] != mt:
            return 'trace differs before the fault'
        if any(e[0] != 'disp' for e in ct[len(mt):]):
            return 'trace: non-disposal events after the fault'
        return 'agree'
    if mo == 'RACE':
        return 'agree' if co == 'RACE' else f'outcome model=RACE compiler={co}'
    return f'outcome model={mo} compiler={co}'

# ----------------------------------------------------------------------------- one program

# r35's known deviations, switched off in the second model configuration so that the residual
# disagreements stand out: #5582 (§28.10 unmarked using var) and the K8.4 temporary-escape
# admission (FINDINGS.md F1; with the knob off the model treats the escaped borrow as a borrow of
# a boundary-disposed temporary, so a later use faults — which level 2 then checks).
R35_GAPS = dict(S28_10_reject_unmarked_pin=False, K8_4_reject_escape=False)


def level1(mk, mrej, mw, ck, crej, cw, call):
    if mk == 'reject' and ck == 'reject':
        return 'agree' if mrej in call else 'reject-family-differs'
    if mk == 'reject' or ck == 'reject':
        return 'disagree'
    if mk == ck and mw == cw:
        return 'agree'
    return 'disagree'


def evaluate(prog, workdir, tag):
    key = bzcore.key(prog)
    m = model_eval(prog)
    mk, mrej, mw = model_families(m)
    with bzcore.mutated(R35_GAPS):
        m35 = model_eval(prog)
    mk35, mrej35, mw35 = model_families(m35)
    src = os.path.join(workdir, f'{tag}.bzl')
    with open(src, 'w') as f:
        f.write(bzlprint.render(prog))
    binc = os.path.join(workdir, f'{tag}.chk')
    rc, errs, warns, notes, raw = compile_one(src, binc, 'checked')
    ck, crej, cw, cmsgs, call = compiler_verdict(rc, errs, warns)
    rec = dict(key=key, model=dict(kind=mk, reject=mrej, warns=mw, raw=m['verdict'], rawwarns=m['warns'], bad=m.get('bad', [])),
               model35=dict(kind=mk35, reject=mrej35, warns=mw35, raw=m35['verdict'], rawwarns=m35['warns'], bad=m35.get('bad', [])),
               compiler=dict(kind=ck, reject=crej, families=call, warns=cw, msgs=cmsgs, rc=rc,
                             preamble_errors=[e for e in errs if e['line'] < bzlprint.BODY_FIRST_LINE]))
    rec['l1'] = level1(mk, mrej, mw, ck, crej, cw, call)
    rec['l1_35'] = level1(mk35, mrej35, mw35, ck, crej, cw, call)
    if mk == 'reject' and mk35 != 'reject':
        m, mk = m35, mk35          # level 2 under the r35-gap model where the spec model rejects
    # level 2
    if mk != 'reject' and ck != 'reject':
        paths = []
        for mr in m['runs']:
            cr = run_binary(binc, mr['choices'], mr['sent'])
            paths.append(dict(choices=mr['choices'], model=mr, compiler=cr, verdict=compare_run(mr, cr)))
        rec['l2'] = 'agree' if all(p['verdict'] == 'agree' for p in paths) else 'disagree'
        rec['paths'] = paths
        # unchecked build: supplementary
        binu = os.path.join(workdir, f'{tag}.unc')
        rcu, *_ = compile_one(src, binu, 'unchecked')
        unc = []
        if rcu == 0:
            for mr in m['runs']:
                cr = run_binary(binu, mr['choices'], mr['sent'])
                unc.append(dict(choices=mr['choices'], outcome=cr['outcome'], trace_ok=(cr['trace'][:len(mr['trace'])] == mr['trace'])))
        rec['unchecked'] = dict(rc=rcu, paths=unc)
        for p in (binc, binu):
            try: os.remove(p)
            except OSError: pass
    elif mk == 'reject' and ck != 'reject':
        # the model rejects, the compiler accepts: run the compiler's program on the first path (no
        # model path vector exists) in both modes, as evidence of what the accepted program does
        cr = run_binary(binc, '')
        rec['compiler_only'] = dict(checked=dict(outcome=cr['outcome'], trace=cr['trace']))
        binu = os.path.join(workdir, f'{tag}.unc')
        rcu, *_ = compile_one(src, binu, 'unchecked')
        if rcu == 0:
            cu = run_binary(binu, '')
            rec['compiler_only']['unchecked'] = dict(outcome=cu['outcome'], trace=cu['trace'])
        for p in (binc, binu):
            try: os.remove(p)
            except OSError: pass
    else:
        try: os.remove(binc)
        except OSError: pass
    os.remove(src)
    return rec

# ----------------------------------------------------------------------------- sweeps

def enum(a, vs, feats):
    for prog in bzcore.enum_programs(vs, a.len, feats, a.with_if, a.with_try, a.with_while, a.body):
        if a.no_fmove and any(s[0] == 'fmove' for s in bzcore.iter_stmts(prog)):
            continue
        yield prog


def sample(progs, n, seed):
    if n is None:
        yield from progs; return
    rnd = random.Random(seed)
    res = []
    for i, p in enumerate(progs):          # reservoir sampling, deterministic under the seed
        if len(res) < n:
            res.append(p)
        else:
            j = rnd.randrange(i + 1)
            if j < n: res[j] = p
    yield from res


def run_sweep(progs, out, jobs, label):
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, 'work'); os.makedirs(work, exist_ok=True)
    results = os.path.join(out, 'results.jsonl')
    t0 = time.time(); n = 0
    with open(results, 'a') as fo, ThreadPoolExecutor(max_workers=jobs) as ex:
        def job(ip):
            i, prog = ip
            tag = hashlib.sha1(bzcore.key(prog).encode()).hexdigest()[:12]
            try:
                return evaluate(prog, work, tag)
            except Exception as e:   # record, never abort a sweep
                return dict(key=bzcore.key(prog), error=repr(e))
        for rec in ex.map(job, enumerate(progs)):
            rec['sweep'] = label
            fo.write(json.dumps(rec, ensure_ascii=False) + '\n'); n += 1
            if n % 200 == 0:
                print(f'  {n} programs, {time.time() - t0:.0f} s', file=sys.stderr, flush=True)
    print(f'{label}: {n} programs in {time.time() - t0:.0f} s -> {results}', file=sys.stderr)


def summarize(results):
    c = Counter(); l1 = Counter(); l2 = Counter(); unm = Counter()
    for line in open(results):
        r = json.loads(line)
        if 'error' in r: c['harness-error'] += 1; continue
        c['programs'] += 1
        c['model-' + r['model']['kind']] += 1
        c['compiler-' + r['compiler']['kind']] += 1
        l1[r['l1']] += 1
        if 'l2' in r: l2[r['l2']] += 1
        for f in [r['compiler']['reject']] + r['compiler']['warns']:
            if f and f.startswith('UNMAPPED'): unm[f] += 1
    return c, l1, l2, unm


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['selftest', 'sweep', 'one', 'summary'])
    ap.add_argument('prog', nargs='?')
    ap.add_argument('--vars', type=int, default=3)
    ap.add_argument('--len', type=int, default=4)
    ap.add_argument('--features', default='all')
    ap.add_argument('--if', dest='with_if', action='store_true')
    ap.add_argument('--try', dest='with_try', action='store_true')
    ap.add_argument('--while', dest='with_while', action='store_true')
    ap.add_argument('--body', type=int, default=2)
    ap.add_argument('--sample', type=int)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--no-fmove', action='store_true')
    ap.add_argument('--out', default=os.path.join(HERE, 'runs', 'default'))
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--label')
    a = ap.parse_args()
    if a.cmd == 'summary':
        c, l1, l2, unm = summarize(os.path.join(a.out, 'results.jsonl'))
        print(dict(c)); print('level1', dict(l1)); print('level2', dict(l2)); print('unmapped', dict(unm)); return
    if a.cmd == 'one':
        prog = parse_key(a.prog)
        print(bzlprint.render(prog))
        os.makedirs(a.out, exist_ok=True)
        print('===JSON==='); print(json.dumps(evaluate(prog, a.out, 'one'), ensure_ascii=False)); return
    if a.cmd == 'selftest':
        progs = [p for _, p, *_ in bzcore.SELFTEST]
        run_sweep(progs, a.out, a.jobs, 'selftest'); return
    vs = ['a', 'b', 'c', 'd'][:a.vars]
    feats = bzcore.FEATURE_SETS.get(a.features) or set(a.features.split(','))
    label = a.label or f'{a.features}-v{a.vars}l{a.len}' + ''.join(f'-{f}' for f in ('if', 'try', 'while') if getattr(a, 'with_' + f))
    run_sweep(sample(enum(a, vs, feats), a.sample, a.seed), a.out, a.jobs, label)


def parse_key(text):
    """Inverse of bzcore.oneline for straight-line programs plus the block forms it prints."""
    lines = [l.strip() for l in text.split(' / ')]
    pos = 0
    def body():
        nonlocal pos
        out = []
        while pos < len(lines):
            l = lines[pos]
            if l in ('}', '} else {') or l.startswith('} catch'):
                return out
            pos += 1
            if l == 'if {':
                a1 = body(); assert lines[pos] == '} else {'; pos += 1
                a2 = body(); assert lines[pos] == '}'; pos += 1
                out.append(('if', a1, a2))
            elif l == '{':
                b = body(); pos += 1; out.append(('block', b))
            elif l == 'try {':
                b = body(); e = lines[pos].split()[2]; pos += 1
                h = body(); pos += 1; out.append(('try', b, e, h))
            elif l == 'while {':
                b = body(); pos += 1; out.append(('while', b))
            else:
                out.append(tuple(l.split()))
        return out
    return body()


if __name__ == '__main__':
    main()
