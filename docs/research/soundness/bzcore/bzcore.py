#!/usr/bin/env python3
"""bzcore — a toy executable model of the ownership kernel's first-order core.

A deliberately small Python rendering of proposal 0047's Layer-1 calculus (spec
§28.3.1, v0.344): A-normal statements over class-typed locals, a flow-sensitive
checker that implements K1 (states), K3 (consumes and their post-states), K4.2
(site-announced kills), K5 (the freeze, over a backward liveness pass), K7 (the
join) and K3.14 (affine scope-end disposal); a small-step interpreter with a
flagged heap (K4.1's backstop) that emits disposal events; a bounded enumerator;
and a rule-mutation harness.

It is a research prototype, not a reference model: no fields (K9), no frames
(K6), no temporaries (K8), no exceptions (K3.15). It exists to show, on ~10^5
generated programs, (a) that a checker written from the kernel text upholds
exactly-once disposal and no-silent-use-after-kill on everything it accepts,
(b) what each kernel clause is load-bearing FOR (delete it and see what breaks),
and (c) how a differential oracle against bezelc or the corpora would be shaped.

Statements (variables a, b, c):
    new x        var x := new C                   fresh production, owned (K1.5)
    brw y x      var y := x                       unmarked read = borrow of x (K3.13)
    move y x     var y := move x                  marked transfer (K3.4; frozen, K5.1)
    asg x        x := new C                       drop-on-overwrite (K3.11; frozen, K5.2)
    disp x       x.Dispose()                      site-announced kill (K3.9; unfrozen, K5.3)
    send x       Domain.Spawn(W, move x)          K3.3 consume into another domain: the
                                                  object stays alive, no flag is ever set
                                                  (K4.3's domain clause) — the v0.180 shape
    use x        x.Member()                       a read through the binding
    decl x       var x: C;                        uninitialized; adopts its first assignment (§7.9, K1 aux state)
    asgb x y     x := y                           borrow-RHS assignment: state-matched (K3.11); an uninit x adopts
    if {..}{..}  nondeterministic branch          K7 meet at the join
    { .. }       a block                          scope-end disposal (K3.14)

Verdicts: reject(rule) | warn(rules) | accept.  Run: ok | FAULT (checked-mode
DisposedAccessException, deterministic, K4.1) | RACE (a read of an object another
domain owns: the unsound state T3 forbids) | STUCK (a read of a moved binding).

Usage:
    python3 -I bzcore.py examples              # the spec's worked shapes, with verdict and trace
    python3 -I bzcore.py enumerate [--vars 3] [--len 5] [--if]
    python3 -I bzcore.py mutate    [--vars 3] [--len 5] [--if]
"""
import argparse, itertools, sys
from collections import Counter

# ----------------------------------------------------------------------------- rule knobs
# Each knob names the kernel clause it embodies. The mutation harness flips them.
RULES = dict(
    K5_freeze_transfers=True,      # K5.1: move/send of a binding under a live borrow is rejected
    K5_freeze_overwrite=True,      # K5.2: drop-on-overwrite of a binding under a live borrow is rejected
    K5_unfreeze_dispose=True,      # K5.3: an explicit Dispose() under a live borrow is admitted
    K7_mixed_rejects=True,         # K7.3: owned ⊓ borrowed is rejected (False: demote to borrowed)
    K7_maybe=True,                 # K7.4: owned ⊓ moved/disposed = maybe-consumed (False: owned)
    K3_13_read_lends=True,         # K3.13: an unmarked read never consumes (False: it moves)
    K1_3_moved_unreadable=True,    # K1.3: a read of a moved-from binding is rejected
    scope_end_per_path=True,       # K3.14 with K7.4: scope-end disposal of maybe is per path
    warn_use_after_dispose=True,   # §28.2: straight-line use after an explicit kill warns
)

# ----------------------------------------------------------------------------- syntax
# A program is a list of statements; a statement is a tuple:
#   ('new',x) ('brw',y,x) ('move',y,x) ('asg',x) ('disp',x) ('send',x) ('use',x)
#   ('if', [stmts], [stmts])  ('block', [stmts])

def show(prog, ind=''):
    out = []
    for s in prog:
        if s[0] == 'if':
            out.append(ind + 'if {'); out += show(s[1], ind + '  ')
            out.append(ind + '} else {'); out += show(s[2], ind + '  '); out.append(ind + '}')
        elif s[0] == 'block':
            out.append(ind + '{'); out += show(s[1], ind + '  '); out.append(ind + '}')
        else:
            out.append(ind + ' '.join(s))
    return out

# ----------------------------------------------------------------------------- liveness
# A backward pass: at each statement, the set of bindings read later in the body
# (K5.5: liveness is body-local). `uses(s)` are the bindings a statement reads.

def reads(s):
    k = s[0]
    if k in ('brw', 'move'):
        return {s[2]}
    if k in ('use', 'disp', 'send'):
        return {s[1]}
    if k == 'asgb':
        return {s[2]}
    return set()

def defs(s):
    return {s[1]} if s[0] in ('new', 'brw', 'move', 'asg', 'asgb', 'decl') else set()

def annotate(prog, live_out):
    """Returns (live_in, [live-out set after each statement]) — the backward pass."""
    outs = [None] * len(prog)
    live = set(live_out)
    for i in range(len(prog) - 1, -1, -1):
        s = prog[i]
        outs[i] = frozenset(live)
        if s[0] == 'if':
            l1, _ = annotate(s[1], live); l2, _ = annotate(s[2], live)
            live = l1 | l2
        elif s[0] == 'block':
            live, _ = annotate(s[1], live)
        else:
            live = (live - defs(s)) | reads(s)
    return frozenset(live), outs

def live_after(prog, live_out):
    return annotate(prog, live_out)[1]

# ----------------------------------------------------------------------------- the checker
# Γ maps a binding to ('own',) | ('brw', frozenset(sources)) | ('moved',) |
# ('disposed',) | ('maybe',).  Scopes track which bindings each block declares.

class Reject(Exception):
    pass

STATIC = {}   # id(body) -> Γ at the body's end: what the compiler would emit scope-end disposal from

def frozen(x, G, live):
    """K5: some live-out binding is a borrow whose source set contains x."""
    for y in live:
        st = G.get(y)
        if st and st[0] == 'brw' and x in st[1]:
            return True
    return False

def check(prog, G=None, live_out=frozenset()):
    """Returns (verdict, warnings, G'). verdict in {'accept','reject:<rule>'}."""
    G = dict(G or {})
    warns = []
    STATIC.clear()
    try:
        G2 = check_body(prog, G, live_out, warns)
    except Reject as r:
        return f'reject:{r.args[0]}', warns, None
    return 'accept', warns, G2

def check_body(prog, G, live_out, warns):
    la = live_after(prog, live_out)
    declared = []
    for i, s in enumerate(prog):
        live = la[i]
        k = s[0]
        if k == 'new':
            G[s[1]] = ('own',); declared.append(s[1])
        elif k == 'decl':
            G[s[1]] = ('uninit',); declared.append(s[1])
        elif k == 'asgb':
            x, y = s[1], s[2]
            sy = G[y]
            if sy[0] in ('moved', 'maybe', 'uninit') and RULES['K1_3_moved_unreadable']:
                raise Reject('K1.3-read-of-moved')
            if sy[0] == 'disposed' and RULES['warn_use_after_dispose']:
                warns.append('§28.2-use-after-dispose')
            src = sy[1] if sy[0] == 'brw' else frozenset({y})
            sx = G[x]
            if sx[0] not in ('uninit', 'brw'):
                raise Reject('state-match')               # an owned-state LHS takes no borrow (K1.7 + K3.11)
            G[x] = ('brw', src)
        elif k == 'brw':
            y, x = s[1], s[2]
            st = G[x]
            if st[0] in ('moved', 'maybe', 'uninit') and RULES['K1_3_moved_unreadable']:
                raise Reject('K1.3-read-of-moved')
            if not RULES['K3_13_read_lends'] and st[0] == 'own':
                G[x] = ('moved',); G[y] = ('own',)       # the mutated language: reads move
            elif st[0] == 'own' or st[0] == 'disposed':
                if st[0] == 'disposed' and RULES['warn_use_after_dispose']:
                    warns.append('§28.2-use-after-dispose')
                G[y] = ('brw', frozenset({x}))
            elif st[0] == 'brw':
                G[y] = ('brw', st[1])                      # transitive re-borrow: same sources (K2.2)
            else:
                G[y] = ('brw', frozenset({x}))
            declared.append(y)
        elif k in ('move', 'send'):
            x = s[2] if k == 'move' else s[1]
            st = G[x]
            if st[0] != 'own':
                raise Reject('K3-consume-of-non-owned')
            if RULES['K5_freeze_transfers'] and frozen(x, G, live):
                raise Reject('K5.1-frozen-transfer')
            G[x] = ('moved',)
            if k == 'move':
                G[s[1]] = ('own',); declared.append(s[1])
        elif k == 'asg':
            x = s[1]; st = G[x]
            if st[0] == 'brw':
                raise Reject('state-match')               # a borrow LHS takes no owned RHS
            if st[0] == 'own' and RULES['K5_freeze_overwrite'] and frozen(x, G, live):
                raise Reject('K5.2-frozen-overwrite')
            G[x] = ('own',)                                # moved/disposed/maybe LHS: owned-state, displaces nothing or per path
        elif k == 'disp':
            x = s[1]; st = G[x]
            if st[0] in ('moved', 'maybe', 'uninit'):
                raise Reject('K1.3-read-of-moved')
            if st[0] == 'brw':
                raise Reject('§28.6-dispose-of-borrow')
            if st[0] == 'disposed':
                warns.append('§28.2-second-dispose')
            if not RULES['K5_unfreeze_dispose'] and frozen(x, G, live):
                raise Reject('mutant-frozen-dispose')
            G[x] = ('disposed',)
        elif k == 'use':
            x = s[1]; st = G[x]
            if st[0] in ('moved', 'maybe', 'uninit') and RULES['K1_3_moved_unreadable']:
                raise Reject('K1.3-read-of-moved')
            if st[0] == 'disposed' and RULES['warn_use_after_dispose']:
                warns.append('§28.2-use-after-dispose')
        elif k == 'if':
            G1 = check_body(s[1], dict(G), live, warns)
            G2 = check_body(s[2], dict(G), live, warns)
            G = meet(G1, G2, set(G))
        elif k == 'block':
            Gi = check_body(s[1], dict(G), live, warns)
            G = {v: Gi[v] for v in G}                      # inner declarations leave scope
    # scope end: declared bindings are released (K3.14) per their static state; nothing to check
    STATIC[id(prog)] = {v: G[v] for v in declared}
    return G

def meet(G1, G2, outer):
    G = {}
    for v in outer:
        a, b = G1[v], G2[v]
        if a == b:
            G[v] = a
        elif a[0] == 'brw' and b[0] == 'brw':
            G[v] = ('brw', a[1] | b[1])                    # K7.2 / K2.2
        elif {a[0], b[0]} == {'own', 'brw'}:
            if RULES['K7_mixed_rejects']:
                raise Reject('K7.3-mixed-merge')
            G[v] = a if a[0] == 'brw' else b               # the mutant: demote
        elif 'own' in (a[0], b[0]) and ({a[0], b[0]} & {'moved', 'disposed', 'maybe', 'uninit'}):
            G[v] = ('maybe',) if RULES['K7_maybe'] else ('own',)
        else:
            G[v] = ('maybe',)                              # moved ⊓ disposed and the like
    return G

# ----------------------------------------------------------------------------- the machine

class Run:
    def __init__(self, oracle):
        self.heap = {}        # loc -> 'live' | 'disposed' | 'foreign'
        self.env = {}         # var -> (loc, dtag)   dtag: own | brw | moved | disposed
        self.events = []      # ('disp', loc) | ('redisp', loc) | ('fault'|'race'|'stuck', var)
        self.oracle = oracle  # iterator of branch choices
        self.n = 0
        self.outcome = 'ok'

    def alloc(self):
        self.n += 1; self.heap[self.n] = 'live'; return self.n

    def dispose(self, loc, explicit):
        if loc is None:                                    # disposing an uninitialized binding
            self.events.append(('redisp', None, 'MACHINERY')); return
        if self.heap[loc] == 'live':
            self.heap[loc] = 'disposed'; self.events.append(('disp', loc))
        elif self.heap[loc] == 'disposed':
            self.events.append(('redisp', loc, 'explicit' if explicit else 'MACHINERY'))
        else:
            self.events.append(('race', 'dispose-of-foreign')); self.outcome = 'RACE'

    def read(self, var):
        loc, tag = self.env[var]
        if tag == 'moved' or loc is None:
            self.events.append(('stuck', var)); self.outcome = 'STUCK'; return
        f = self.heap[loc]
        if f == 'disposed':
            self.events.append(('fault', var)); self.outcome = 'FAULT'
        elif f == 'foreign':
            self.events.append(('race', var)); self.outcome = 'RACE'

    def body(self, prog):
        declared = []
        for s in prog:
            if self.outcome in ('FAULT', 'RACE', 'STUCK'):
                return                     # a fault ends the program (checked mode)
            k = s[0]
            if k == 'new':
                self.env[s[1]] = (self.alloc(), 'own'); declared.append(s[1])
            elif k == 'decl':
                self.env[s[1]] = (None, 'uninit'); declared.append(s[1])
            elif k == 'asgb':
                self.read(s[2])
                if self.outcome != 'ok':
                    return
                loc, _ = self.env[s[2]]
                self.env[s[1]] = (loc, 'brw')
            elif k == 'brw':
                loc, tag = self.env[s[2]]
                if tag in ('moved', 'uninit'):
                    self.events.append(('stuck', s[2])); self.outcome = 'STUCK'; return
                if not RULES['K3_13_read_lends'] and tag == 'own':
                    self.env[s[2]] = (loc, 'moved'); self.env[s[1]] = (loc, 'own')
                else:
                    self.env[s[1]] = (loc, 'brw')
                declared.append(s[1])
            elif k == 'move':
                loc, tag = self.env[s[2]]
                self.env[s[2]] = (loc, 'moved'); self.env[s[1]] = (loc, 'own'); declared.append(s[1])
            elif k == 'send':
                loc, tag = self.env[s[1]]
                self.heap[loc] = 'foreign'; self.env[s[1]] = (loc, 'moved')
            elif k == 'asg':
                loc, tag = self.env[s[1]]
                new = self.alloc()                         # RHS first (K9.5's order)
                if tag == 'own':
                    self.dispose(loc, explicit=False)      # drop-on-overwrite
                self.env[s[1]] = (new, 'own')
            elif k == 'disp':
                self.read(s[1])
                if self.outcome == 'ok':
                    loc, tag = self.env[s[1]]
                    self.dispose(loc, explicit=True); self.env[s[1]] = (loc, 'disposed')
                else:
                    return
            elif k == 'use':
                self.read(s[1])
            elif k == 'if':
                self.body(s[1] if next(self.oracle) else s[2])
            elif k == 'block':
                self.body(s[1])
        # scope end, in reverse establishment order, driven by the STATIC classification (what a
        # compiler emits): own → dispose; brw/moved/disposed → nothing; maybe → the per-path dtag.
        st_tab = STATIC.get(id(prog), {})
        for v in reversed(declared):
            loc, tag = self.env[v]
            st = st_tab.get(v, (tag,))[0]
            if st == 'own':
                if tag == 'own':
                    self.dispose(loc, explicit=False)
                elif tag == 'moved':
                    self.dispose(loc, explicit=False)       # disposing what another holder owns
                    self.events.append(('redisp', loc, 'MACHINERY'))
                elif tag == 'disposed':
                    self.events.append(('redisp', loc, 'MACHINERY'))
                elif tag == 'brw':
                    self.dispose(loc, explicit=False)       # a borrow classified owned: frees the source's object
                    self.events.append(('redisp', loc, 'MACHINERY'))
            elif st == 'maybe':
                if tag == 'own' or (tag == 'disposed' and not RULES['scope_end_per_path']):
                    self.dispose(loc, explicit=False)
            # brw / moved / disposed / uninit: the compiler emits nothing; a dtag of own here is a leak

def count_branches(prog):
    n = 0
    for s in prog:
        if s[0] == 'if':
            n += 1 + count_branches(s[1]) + count_branches(s[2])
        elif s[0] == 'block':
            n += count_branches(s[1])
    return n

def run_all(prog):
    """Every path (every assignment of the branch oracle). Returns list of Runs."""
    k = count_branches(prog)
    runs = []
    for bits in itertools.product([True, False], repeat=k):
        r = Run(iter(bits)); r.body(prog); runs.append(r)
    return runs

# ----------------------------------------------------------------------------- the theorems

def judge(prog):
    """T1: exactly one disp per non-foreign allocation on every complete path, and no
    MACHINERY re-dispose. T2/T3: no RACE, no STUCK (a FAULT is the backstop doing its job)."""
    bad = []
    for r in run_all(prog):
        if r.outcome in ('RACE', 'STUCK'):
            bad.append(r.outcome)
            continue
        if r.outcome == 'FAULT':
            continue                                  # T2: deterministic fault, admitted
        disp = Counter(l for e in r.events if e[0] == 'disp' for l in [e[1]])
        for loc, f in r.heap.items():
            if f == 'foreign':
                if disp[loc]:
                    bad.append('T1-disposed-then-sent')
            elif disp[loc] != 1:
                bad.append('T1-leak' if disp[loc] == 0 else 'T1-double')
        if any(e[0] == 'redisp' and e[2] == 'MACHINERY' for e in r.events):
            bad.append('T1-machinery-redispose')
    return bad

# ----------------------------------------------------------------------------- enumeration

def atoms(vs):
    for x in vs:
        yield ('new', x); yield ('decl', x); yield ('asg', x); yield ('disp', x); yield ('send', x); yield ('use', x)
        for y in vs:
            if y != x:
                yield ('brw', y, x); yield ('move', y, x); yield ('asgb', x, y)

def wellformed_step(declared, s):
    """Prune programs that are ill-formed rather than rejected: every read names a
    declared binding, every declaration a fresh one (at most one declaration per name)."""
    k = s[0]
    if k in ('new', 'decl'):
        return s[1] not in declared, declared | {s[1]}
    if k in ('brw', 'move'):
        return (s[2] in declared and s[1] not in declared), declared | {s[1]}
    if k == 'asgb':
        return s[1] in declared and s[2] in declared, declared
    return s[1] in declared, declared

def enum_straight(vs, n, declared=frozenset()):
    """All well-formed straight-line programs of length exactly n."""
    if n == 0:
        yield []
        return
    for s in atoms(vs):
        ok, d2 = wellformed_step(declared, s)
        if ok:
            for rest in enum_straight(vs, n - 1, d2):
                yield [s] + rest

def enum_programs(vs, maxlen, with_if):
    for n in range(1, maxlen + 1):
        yield from enum_straight(vs, n)
    if with_if:
        # prefix (1..maxlen-2) ; if {arm1 (0..1)} {arm2 (0..1)} ; suffix (0..1) — arms declare nothing
        for pre in itertools.chain(*(enum_straight(vs, n) for n in range(1, max(2, maxlen - 1)))):
            d = frozenset(s[1] for s in pre if s[0] in ('new', 'brw', 'move', 'decl'))
            arms = [a for n in (0, 1) for a in enum_straight(vs, n, d)
                    if not any(s[0] in ('new', 'brw', 'move', 'decl') for s in a)]
            for a1 in arms:
                for a2 in arms:
                    if a1 >= a2:
                        continue
                    for suf in itertools.chain([[]], enum_straight(vs, 1, d)):
                        yield pre + [('if', a1, a2)] + suf

# ----------------------------------------------------------------------------- examples

EXAMPLES = [
    ('v0.180 probe (rationale §5.1): borrow, then transfer to a domain, then use the borrow',
     [('new', 'x'), ('brw', 'b', 'x'), ('send', 'x'), ('use', 'b')], 'reject:K5.1-frozen-transfer'),
    ('0047 §5.3 example: var b := x; var y := move x; use b',
     [('new', 'x'), ('brw', 'b', 'x'), ('move', 'y', 'x'), ('use', 'b')], 'reject:K5.1-frozen-transfer'),
    ('K5.3: explicit Dispose() under a live borrow is admitted; the later use faults deterministically',
     [('new', 'x'), ('brw', 'b', 'x'), ('disp', 'x'), ('use', 'b')], 'accept'),
    ('K5.2 (§28.7 Overwrite while borrowed): drop-on-overwrite under a live borrow is frozen',
     [('new', 'x'), ('brw', 'b', 'x'), ('asg', 'x'), ('use', 'b')], 'reject:K5.2-frozen-overwrite'),
    ('K7.3 (§28.3.3 mixed merge): owned on one arm, borrowed on the other, is rejected at the join',
     [('decl', 'x'), ('new', 'a'), ('if', [('asg', 'x')], [('asgb', 'x', 'a')]), ('use', 'x')], 'reject:K7.3-mixed-merge'),
    ('§28.3.3 Pattern 1: equalize to borrowed — both arms bind x to a source that outlives the merge',
     [('decl', 'x'), ('new', 'a'), ('new', 'b'), ('if', [('asgb', 'x', 'a')], [('asgb', 'x', 'b')]), ('use', 'x')], 'accept'),
    ('K7.4: a conditional kill joins to maybe-consumed; the later read is rejected',
     [('new', 'a'), ('if', [('disp', 'a')], []), ('use', 'a')], 'reject:K1.3-read-of-moved'),
    ('K3.14 affinity: an undischarged owned local is disposed at scope end (trace has one disp)',
     [('new', 'a'), ('block', [('new', 'b'), ('use', 'b')]), ('use', 'a')], 'accept'),
    ('§28.2: straight-line use after an explicit kill is a warning, and faults in checked mode',
     [('new', 'a'), ('disp', 'a'), ('use', 'a')], 'accept'),
]

def cmd_examples():
    for title, prog, expect in EXAMPLES:
        v, w, _ = check(prog)
        print(f'--- {title}\n    ' + '\n    '.join(show(prog)))
        print(f'    verdict: {v}  warnings: {sorted(set(w))}  (expected {expect}) {"OK" if v == expect else "MISMATCH"}')
        if v == 'accept':
            for r in run_all(prog):
                print(f'    run: {r.outcome:5}  events: {r.events}')

# ----------------------------------------------------------------------------- enumerate / mutate

def sweep(vs, maxlen, with_if):
    stats = Counter(); viol = []
    accepted = {}
    for prog in enum_programs(vs, maxlen, with_if):
        v, w, _ = check(prog)
        stats['programs'] += 1
        if v == 'accept':
            stats['accept'] += 1
            if w: stats['accept-with-warning'] += 1
            bad = judge(prog)
            key = tuple(show(prog))
            accepted[key] = (prog, bad)
            if bad:
                stats['VIOLATIONS'] += 1
                if len(viol) < 3: viol.append((prog, sorted(set(bad))))
        else:
            stats[v] += 1
    return stats, viol, accepted

def cmd_enumerate(vs, maxlen, with_if):
    stats, viol, _ = sweep(vs, maxlen, with_if)
    print('baseline kernel:', dict(stats))
    for prog, bad in viol:
        print('VIOLATION', bad); print('\n'.join(show(prog, '    ')))
    print('T1/T2/T3 hold on every accepted program' if not viol else 'MODEL DEFECT: see above')

MUTANTS = {
    'K5.1 deleted (transfers unfrozen) — the pre-v0.180 language': dict(K5_freeze_transfers=False),
    'K5.2 deleted (drop-on-overwrite unfrozen)': dict(K5_freeze_overwrite=False),
    'K5.3 flipped (explicit Dispose() frozen too)': dict(K5_unfreeze_dispose=False),
    'K7.3 weakened (owned ⊓ borrowed demotes to borrowed)': dict(K7_mixed_rejects=False),
    'K7.4 weakened (owned ⊓ moved = owned)': dict(K7_maybe=False),
    'K1.3 deleted (moved-from bindings readable)': dict(K1_3_moved_unreadable=False),
    'K3.13 flipped (an unmarked read consumes)': dict(K3_13_read_lends=False),
    'scope end disposes explicitly-disposed bindings again (K3.9 post-state ignored)': dict(scope_end_per_path=False),
    '§28.2 use-after-dispose warning dropped': dict(warn_use_after_dispose=False),
}

def cmd_mutate(vs, maxlen, with_if):
    base_stats, base_viol, base_acc = sweep(vs, maxlen, with_if)
    assert not base_viol, 'the baseline must be sound before mutating'
    print(f'baseline: {base_stats["programs"]} programs, {base_stats["accept"]} accepted '
          f'({base_stats["accept-with-warning"]} with warnings), 0 violations\n')
    print('| mutation | verdicts changed | newly accepted | of which unsound | newly rejected | unsound among all accepted | witness |')
    print('|---|---|---|---|---|---|---|')
    for name, flips in MUTANTS.items():
        saved = dict(RULES); RULES.update(flips)
        try:
            stats, viol, acc = sweep(vs, maxlen, with_if)
        finally:
            RULES.update(saved)
        new_acc = [k for k in acc if k not in base_acc]
        new_rej = [k for k in base_acc if k not in acc]
        unsound = [acc[k] for k in new_acc if acc[k][1]]
        unsound_all = [acc[k] for k in acc if acc[k][1]]
        # a mutant that keeps the verdicts but changes warnings
        witness = ''
        if unsound or unsound_all:
            prog, bad = min(unsound or unsound_all, key=lambda pb: len(show(pb[0])))
            witness = ' / '.join(show(prog)) + ' → ' + ','.join(sorted(set(bad)))
        elif new_acc:
            witness = ' / '.join(new_acc[0]) + ' (sound: the flag backstops it)'
        changed = len(new_acc) + len(new_rej)
        if changed == 0 and stats['accept-with-warning'] != base_stats['accept-with-warning']:
            witness = f'only warnings change: {base_stats["accept-with-warning"]} → {stats["accept-with-warning"]} warned programs'
        print(f'| {name} | {changed} | {len(new_acc)} | {len(unsound)} | {len(new_rej)} | {len(unsound_all)} | {witness} |')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['examples', 'enumerate', 'mutate'])
    ap.add_argument('--vars', type=int, default=3)
    ap.add_argument('--len', type=int, default=5)
    ap.add_argument('--if', dest='with_if', action='store_true')
    a = ap.parse_args()
    vs = ['a', 'b', 'c', 'd'][:a.vars]
    if a.cmd == 'examples':
        cmd_examples()
    elif a.cmd == 'enumerate':
        cmd_enumerate(vs, a.len, a.with_if)
    else:
        cmd_mutate(vs, a.len, a.with_if)
