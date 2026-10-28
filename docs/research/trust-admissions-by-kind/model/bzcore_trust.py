#!/usr/bin/env python3
"""bzcore_trust — bzcore L1 extended with HOLDER statements for the trust-shape study (a COPY; the original is
docs/research/soundness/bzcore-l1/bzcore.py). Added over the L1 fragment:

    newk x y      var x := new K(y);     K stores its plain parameter in a NON-owned field (the K4.3 field-held
                                           admission, §28.3 clause 3; kind 3 "short-lived helper" when x is a local)
    kuse x        x.Show();              a member access THROUGH the stored borrow (reads y's object)
    link x        x.f.Attach(x);         the owned child Q of holder x gets a back-pointer to its owner (kind 1)
    puse y        y.OwnerName();         a member access through a child's back-pointer

  Rule knobs: derived_retention (0132: x carries {y} — K5 freezes y's silent kills while x is live, and a kuse x
  that a kill of every carried source dominates is 0205's holder error); freeze_linked_fmove (owner-outlives: a
  linked child may not be moved out of its owner while the owner may die first).

  Original docstring follows.

bzcore (L1) — an executable model of the ownership kernel over proposal 0047's Layer 1.

A Python rendering of the kernel of spec §28.3.1 (v0.345) over 0047 §3.1's A-normal
Layer-1 fragment: class-typed locals, one owned field with the replacing `move` (K9),
unbound owned temporaries at the statement boundary (K8), `throw`/`try` with edge parity
(K3.15), `using var` pins (A5), nondeterministic branches and one-back-edge loops (K7).
Three parts:

  * a flow-sensitive CHECKER written clause by clause from §28.3.1 — every branch of
    `check_body` names the K id it implements, so a reviewer can read it against the spec
    rule for rule;
  * a MACHINE (small-step interpreter) whose heap carries K4.1's disposed flag, a `send`
    sink with no flag (K4.3's domain clause), one establishment-ordered cleanup stack
    shared by the normal and the exception edge (K3.15), and a disposal-event trace;
  * ORACLES over the trace: T1 exactly-once, T2 no silent use-after-kill, T3 one-sink
    isolation, and the retention oracle behind the 2026-10-27 #4602 ruling (a
    "free-in-full" heap variant that reports reads of freed memory).

Plus a bounded deterministic ENUMERATOR, a rule-MUTATION harness, and a rule-change CLI.

The surface it models (variables a, b, c; two classes: `R` with one `owned f: Q` field
and the leaf `Q`; `e` is the catch binding):

    new x          var x := new R(new Q());        fresh production, owned (K1.5)
    decl x         var x: R;                       uninitialized; adopts its first assignment (§28.3.3)
    using x        using var x := new R(new Q());  owned scope pin (A5, §28.10)
    brw y x        var y := x;                     unmarked read = borrow (K3.13), sources = x's value
    move y x       var y := move x;                marked transfer (K3.4; frozen, K5.1)
    usingm y x     using var y := move x;          K3.4 adoption into a pin (§28.10)
    usingb y x     using var y := x;               borrow-pin of a borrow; REJECT over an owned x (§28.10)
    asg x          x := new T();                   drop-on-overwrite (K3.11; frozen, K5.2)
    asgb x y       x := y;                         borrow RHS, state-matched (§28.3.3)
    asgm x y       x := move y;                    marked reassignment (K3.4 + K3.11)
    disp x         x.Dispose();                    site-announced kill (K3.9; unfrozen, K5.3)
    send x         Domain.Spawn(W, move x);        K3.3 consume into another domain: no flag ever (K4.3)
    use x          x.Member();                     a member access through the binding
    fst x          x.f := new Q();                 slot overwrite (K3.11 at a slot; not frozen, K5.6)
    fstm x y       x.f := move y;                  store into an owned field (K3.1; frozen, K5.1)
    fbrw y x       var y := x.f;                   storage read lends; source = the holder (K1.5, K9.1)
    fmove y x      var y := move x.f with new Q(); the replacing move (K9.1, K9.5)
    tuse           new R(new Q()).Member();        unbound temporary, disposed at the boundary (K8.1)
    tdisp          new R(new Q()).Dispose();       K3.9 on a temporary; the boundary reclaims only
    tsend          Domain.Spawn(W, new R(new Q())); a fresh producer in an owned argument position
    tbrw y         var y := new R(new Q()).f;      a borrow of a temporary escapes: REJECT (K8.4)
    throw x        throw x;                        K3.8: keyword-announced consume, unfrozen (K5.3)
    tthrow         throw new R(new Q());           a fresh thrown value
    if {A} {B}     if * then A else B end          nondeterministic branch; K7 meet at the join
    block {S}      do S end do;                    scope end disposal (K3.14)
    try {S} e {H}  try S catch e: R => H end       e borrows the in-flight exception (§31.1); K3.15 parity
    while {S}      while * do S end                one back edge; the machine runs 0, 1 or 2 iterations

Verdicts: accept | accept with warnings | reject:<rule>.  Runs: ok | FAULT (checked-mode
DisposedAccessException, K4.1) | RACE (a read of an object another domain owns) | STUCK (a
read of a moved-from binding) | UAF (free-in-full variant only: a read of freed memory).

Usage:
    python3 -I bzcore.py selftest
    python3 -I bzcore.py enumerate [--features all|locals|...] [--vars 3] [--len 4] [--if] [--try] [--while]
    python3 -I bzcore.py mutate    [same bounds]
    python3 -I bzcore.py check --mutation <name> | --define FILE.py [same bounds]
    python3 -I bzcore.py retention [same bounds]
    python3 -I bzcore.py mutations            # list the mutation table's names

Spec pin: v0.345 (§28.2, §28.3.1–§28.3.3, §28.3.7–§28.3.11, §28.7, §28.8.1, §28.10, §31.1).
No dependencies. Python 3.11.
"""
import argparse, importlib.util, sys, time
from collections import Counter

# ----------------------------------------------------------------------------- rule knobs
# Each knob names the kernel clause it embodies; the mutation harness flips them.
RULES = dict(
    K5_freeze_transfers=True,       # K5.1: a transfer (move/send/field store) under a live borrow is rejected
    K5_freeze_overwrite=True,       # K5.2: drop-on-overwrite of a binding under a live borrow is rejected
    K5_unfreeze_dispose=True,       # K5.3: explicit Dispose() under a live borrow is admitted (False = K5.3-c)
    K7_mixed_rejects=True,          # K7.3: owned ⊓ borrowed is rejected (False: demote to borrowed)
    K7_maybe=True,                  # K7.4: owned ⊓ moved/disposed = maybe-consumed (False: owned)
    K3_13_read_lends=True,          # K3.13: an unmarked read never consumes (False: it moves)
    K1_3_moved_unreadable=True,     # K1.3: a read of a moved-from / maybe / uninit binding is rejected
    release_skips_disposed=True,    # K1 'explicitly-disposed': a release site of a disposed binding emits no disposer call
    warn_use_after_dispose=True,    # §28.2: a dominated same-variable use after an explicit kill warns
    K8_4_reject_escape=True,        # K8.4: a borrow of an unbound temporary may not escape its boundary
    A5_pin=True,                    # A5: a using var is neither reassignable nor movable out
    K3_15_edge_parity=True,         # K3.15: the exception edge performs the same implicit disposal
    K9_5_rhs_first=True,            # K9.5: drop-on-overwrite evaluates the RHS before disposing the displaced value
    K5_3b_borrow='off',             # K5.3-a/b: a use of a borrow of a value explicitly killed on every path: 'off' | 'warn' | 'error'
    K5_3b_samevar='off',            # K5.3-b: the §28.2 same-variable dominated use becomes an error: 'off' | 'error'
    derived_retention=False,        # 0132-style: a holder minted by `newk x y` carries y's sources (K5 + 0205 see it)
    freeze_linked_fmove=False,      # owner-outlives: `fmove c x` is rejected while x's child is linked back to x
    linked_fmove_carries=False,     # owner-outlives, sharp: a linked child moved out CARRIES its owner (kind 1 degrades to kind 3 at the move)
)
# Overridable predicates for user-defined mutations (see `check --define`). Each receives the
# checker's inputs and returns the clause's answer; None means "use the kernel's text".
HOOKS = dict(
    frozen=None,     # frozen(ids, G, live) -> bool            K5's criterion (default: a live borrow sources one of ids)
    meet=None,       # meet(a, b) -> state | raise Reject      K7's cell for two states (default: the K7 table)
)

TYPES_R = 'R'; TYPES_Q = 'Q'; TYPES_K = 'K'

# ----------------------------------------------------------------------------- syntax
# A program is a list of statements; a statement is a tuple:
#   ('new',x) ('decl',x) ('using',x) ('brw',y,x) ('move',y,x) ('usingm',y,x) ('usingb',y,x)
#   ('asg',x) ('asgb',x,y) ('asgm',x,y) ('disp',x) ('send',x) ('use',x)
#   ('fst',x) ('fstm',x,y) ('fbrw',y,x) ('fmove',y,x)
#   ('tuse',) ('tdisp',) ('tsend',) ('tbrw',y) ('throw',x) ('tthrow',)
#   ('if',[A],[B]) ('block',[S]) ('try',[S],e,[H]) ('while',[S])
# A statement's SITE is its position path in the program (a tuple), which also serves as
# the allocation-site identity of the value a producer at that site mints.

DECL_KINDS = ('new', 'decl', 'using', 'brw', 'move', 'usingm', 'usingb', 'fbrw', 'fmove', 'tbrw', 'newk')
PIN_KINDS = ('using', 'usingm', 'usingb')

def show(prog, ind=''):
    out = []
    for s in prog:
        k = s[0]
        if k == 'if':
            out.append(ind + 'if {'); out += show(s[1], ind + '  ')
            out.append(ind + '} else {'); out += show(s[2], ind + '  '); out.append(ind + '}')
        elif k == 'block':
            out.append(ind + '{'); out += show(s[1], ind + '  '); out.append(ind + '}')
        elif k == 'try':
            out.append(ind + 'try {'); out += show(s[1], ind + '  ')
            out.append(ind + '} catch ' + s[2] + ' {'); out += show(s[3], ind + '  '); out.append(ind + '}')
        elif k == 'while':
            out.append(ind + 'while {'); out += show(s[1], ind + '  '); out.append(ind + '}')
        else:
            out.append(ind + ' '.join(s))
    return out

def oneline(prog):
    return ' / '.join(show(prog))

def key(prog):
    return oneline(prog)

# ----------------------------------------------------------------------------- liveness
# A backward pass over one body (K5.5: liveness is body-local). `reads(s)` are the bindings a
# statement reads; `defs(s)` the binding it (re)defines. A `throw` transfers control to the
# innermost enclosing handler, so its live-out is the handler's live-in (`exc_live`).

def reads(s):
    k = s[0]
    if k in ('brw', 'move', 'usingm', 'usingb', 'fbrw', 'fmove', 'asgb', 'asgm'):
        return {s[2]}
    if k in ('use', 'disp', 'send', 'throw', 'fst', 'kuse', 'link', 'puse'):
        return {s[1]}
    if k == 'newk':
        return {s[2]}
    if k == 'fstm':
        return {s[1], s[2]}
    return set()

def defs(s):
    return {s[1]} if s[0] in DECL_KINDS or s[0] in ('asg', 'asgb', 'asgm') else set()

def annotate(prog, live_out, exc_live):
    """Returns (live_in, [live-out set after each statement])."""
    outs = [None] * len(prog)
    live = set(live_out)
    for i in range(len(prog) - 1, -1, -1):
        s = prog[i]; k = s[0]
        if k == 'if':
            outs[i] = frozenset(live)
            l1, _ = annotate(s[1], live, exc_live); l2, _ = annotate(s[2], live, exc_live)
            live = l1 | l2
        elif k == 'block':
            outs[i] = frozenset(live)
            live, _ = annotate(s[1], live, exc_live)
        elif k == 'try':
            outs[i] = frozenset(live)
            lh, _ = annotate(s[3], live, exc_live)          # the handler's live-in (e is defined there)
            ls, _ = annotate(s[1], live, lh)                 # the body: throws jump to the handler
            live = ls
        elif k == 'while':
            outs[i] = frozenset(live)
            live = loop_head_live(s[1], live, exc_live)
        elif k in ('throw', 'tthrow'):
            outs[i] = frozenset(exc_live)
            live = set(exc_live) | reads(s)
        else:
            outs[i] = frozenset(live)
            live = (live - defs(s)) | reads(s)
    return frozenset(live), outs

def loop_head_live(body, live_exit, exc_live):
    """The live set at a loop head: a fixpoint of exit ∪ live_in(body) around the back edge."""
    head = frozenset(live_exit)
    for _ in range(8):
        lb, _ = annotate(body, head, exc_live)
        new = head | lb
        if new == head:
            return head
        head = new
    return head

# ----------------------------------------------------------------------------- the checker
# Γ maps a binding to one of K1's states; every value-carrying state carries the set of
# allocation sites its value may come from (`ids`), which is what borrows name as sources:
#   ('own', ids) | ('brw', srcs, born_stale, src_bindings) | ('moved',) | ('disposed', ids) | ('maybe', ids) | ('uninit',)
# A borrow's `srcs` are value ids (what K5's freeze and K5.3-b's dominance read); its
# `src_bindings` are the named locations it is bounded by (what K2.3's scope comparison reads).
# Γ['__killed'] is the set of value ids an explicit kill (K3.9) has flagged on EVERY path to
# this point (intersected at joins) — the dominance the §28.2 warning and K5.3-b read.

class Reject(Exception):
    pass

class Ctx:
    def __init__(self):
        self.warns = set()       # (site, rule)
        self.pins = set()        # names declared `using var`
        self.types = {}          # name -> 'R' | 'Q'
        self.static = {}         # body path -> Γ of its declared bindings at scope end; throw site -> Γ there
        self.disp_sites = {}     # site -> dict(x, static, frozen, self_live, taint, order, loop)
        self.asg_static = {}     # site -> static state kind of the LHS at an overwrite
        self.releases = []       # (order, x, static kind, loop)  — later release sites of a binding
        self.taint = set()       # value ids that left a slot while a borrow of the holder was live
        self.depth = {}          # name -> nesting depth of the body that declared it (K2.3 scope comparison)
        self.order = 0
        self.throw_stack = []    # per enclosing try: [(Γ at throw site, thrown ids)]
        self.loop_stack = []     # enclosing loop sites

def ids_of(st):
    return st[1] if st[0] in ('own', 'brw', 'disposed', 'maybe') else frozenset()

def carried(y, G):
    """the sources a HOLDER binding carries under derived retention: its own K-carry, or, for a borrow of a
    holder, the carry of the holder bindings it is bounded by (transitively one hop: the fragment's depth)."""
    if not (RULES['derived_retention'] or RULES['linked_fmove_carries']):
        return frozenset()
    c = G.get('__carry', {})
    out = frozenset(c.get(y, frozenset()))
    st = G.get(y)
    if st and st[0] == 'brw':
        for b in st[3]:
            out |= c.get(b, frozenset())
    return out

def frozen(ids, G, live):
    """K5's criterion: some live-out binding is a borrow whose source set meets `ids`, the
    current value(s) of the binding being consumed ("a live local borrow of its current value").
    Under derived retention a live holder that carries one of `ids` freezes it too (0132 + K5.5)."""
    if HOOKS['frozen']:
        return HOOKS['frozen'](ids, G, live)
    for y in live:
        st = G.get(y)
        if st and st[0] == 'brw' and (st[1] & ids):
            return True
        if carried(y, G) & ids:
            return True
    return False

def read_check(x, G, site, ctx):
    """A read of binding x (K1.3, §28.2, and the K5.3-a/b additions).
    K1.3: moved-from, maybe-consumed and uninitialized are unreadable (error).
    §28.2: a read of an explicitly-disposed binding under a dominating kill is a mandatory
    warning (K5.3-b: an error).  K5.3-a/b: a read through a borrow whose every source value
    was explicitly killed on every path, the borrow taken before the kill, warns / errors."""
    st = G[x]
    if st[0] in ('moved', 'maybe', 'uninit'):
        if RULES['K1_3_moved_unreadable']:
            raise Reject('K1.3-read-of-moved')
        return
    if st[0] == 'disposed':
        if RULES['K5_3b_samevar'] == 'error':
            raise Reject('K5.3b-same-variable-dominated-use')
        if RULES['warn_use_after_dispose']:
            ctx.warns.add((site, '§28.2-use-after-dispose'))
    if st[0] == 'brw' and st[1] and not st[2] and st[1] <= G['__killed']:
        if RULES['K5_3b_borrow'] == 'error':
            raise Reject('K5.3b-use-of-killed-borrow')
        if RULES['K5_3b_borrow'] == 'warn':
            ctx.warns.add((site, 'K5.3a-use-of-killed-borrow'))

def consume_check(x, G, site, ctx, live, freeze_rule):
    """A consume of binding x at a transfer row (K3.1, K3.3, K3.4) or at `throw` (K3.8).
    Only an owned binding is consumable (a borrow is K3-consume-of-non-owned); a pin is never
    moved out (A5); a moved/maybe/uninit binding is unreadable (K1.3); an explicitly-disposed
    binding draws §28.2's warning ("a consume exactly as a read does"); the transfer rows are
    frozen under a live borrow of the current value (K5.1), `throw` is not (K5.3).
    Returns the consumed value ids."""
    st = G[x]
    if st[0] in ('moved', 'maybe', 'uninit'):
        if RULES['K1_3_moved_unreadable']:
            raise Reject('K1.3-read-of-moved')
        return frozenset()
    if st[0] == 'brw':
        raise Reject('K3-consume-of-non-owned')
    if x in ctx.pins and RULES['A5_pin']:
        raise Reject('A5-move-out-of-using-var')
    if st[0] == 'disposed':
        if RULES['K5_3b_samevar'] == 'error':
            raise Reject('K5.3b-same-variable-dominated-use')
        if RULES['warn_use_after_dispose']:
            ctx.warns.add((site, '§28.2-use-after-dispose'))
    ids = st[1]
    if freeze_rule and RULES['K5_freeze_transfers'] and frozen(ids, G, live):
        raise Reject('K5.1-frozen-transfer')
    return ids

def borrow_of(x, st, G):
    """The borrow state a read of binding x in state `st` produces (K1.5, K2.2): the sources
    are the binding's current value ids, bounded by the binding x, or a re-borrow's own sources
    and bounds; `born_stale` records a borrow taken after a dominating kill of all its sources
    (then K5.3-b's borrow clause, "taken before that kill", does not apply to it)."""
    if st[0] == 'brw':
        return ('brw', st[1], st[2], st[3])
    ids = ids_of(st)
    return ('brw', ids, bool(ids) and ids <= G['__killed'], frozenset({x}))

def escape_check(x, st, ctx):
    """K2.3 at a local store: a borrow may not reach a binding of wider scope than one of its
    source bindings (the scopes of locals are statically comparable, so this is a rejection, not
    a trust admission); the catch binding's scope is its arm (§31.1)."""
    if st[0] == 'brw' and any(ctx.depth.get(s, 0) > ctx.depth.get(x, 0) for s in st[3]):
        raise Reject('K2.3-borrow-outlives-source')

def check(prog):
    """Returns (verdict, warnings, ctx). verdict in {'accept', 'reject:<rule>'}."""
    ctx = Ctx()
    G = {'__killed': frozenset(), '__carry': {}, '__linked': frozenset()}
    try:
        check_body(prog, (), G, frozenset(), frozenset(), ctx)
    except Reject as r:
        return f'reject:{r.args[0]}', sorted({w[1] for w in ctx.warns}), ctx
    return 'accept', sorted({w[1] for w in ctx.warns}), ctx

def check_body(prog, path, G, live_out, exc_live, ctx, pre_declared=(), depth=0):
    """The statement judgment over one body; returns Γ at the body's end (None if unreachable)."""
    la = annotate(prog, live_out, exc_live)[1]
    declared = list(pre_declared)
    for v in pre_declared: ctx.depth[v] = depth
    loop = ctx.loop_stack[0] if ctx.loop_stack else None
    for i, s in enumerate(prog):
        site = path + (i,)
        live = la[i]
        ctx.order += 1; order = ctx.order
        k = s[0]
        if k == 'new':                                  # K1.5: a fresh production is owned
            G[s[1]] = ('own', frozenset({site})); ctx.types[s[1]] = TYPES_R; declared.append(s[1]); ctx.depth[s[1]] = depth
        elif k == 'decl':                               # §7.9 / §28.3.3: uninitialized, adopts its first assignment
            G[s[1]] = ('uninit',); ctx.types[s[1]] = TYPES_R; declared.append(s[1]); ctx.depth[s[1]] = depth
        elif k == 'using':                              # A5 / §28.10: an owned scope pin
            G[s[1]] = ('own', frozenset({site})); ctx.types[s[1]] = TYPES_R; ctx.pins.add(s[1]); declared.append(s[1]); ctx.depth[s[1]] = depth
        elif k == 'brw':                                # K3.13: an unmarked read of a class-typed binding never consumes
            y, x = s[1], s[2]
            read_check(x, G, site, ctx)
            st = G[x]
            if not RULES['K3_13_read_lends'] and st[0] == 'own':
                G[x] = ('moved',); G[y] = ('own', st[1])    # the mutated language: reads move
            else:
                G[y] = borrow_of(x, st, G)                  # K1.5 + K2.2 (a re-borrow carries the same sources)
            ctx.types[y] = ctx.types[x]; declared.append(y); ctx.depth[y] = depth
        elif k == 'move':                               # K3.4: marked transfer; frozen (K5.1)
            y, x = s[1], s[2]
            ids = consume_check(x, G, site, ctx, live, True)
            G[x] = ('moved',); G[y] = ('own', ids); ctx.types[y] = ctx.types[x]; declared.append(y); ctx.depth[y] = depth
            if x in G['__carry']:
                G['__carry'] = dict(G['__carry']); G['__carry'][y] = G['__carry'][x]
        elif k == 'usingm':                             # K3.4 adoption into a pin (§28.10)
            y, x = s[1], s[2]
            ids = consume_check(x, G, site, ctx, live, True)
            G[x] = ('moved',); G[y] = ('own', ids); ctx.types[y] = ctx.types[x]; ctx.pins.add(y); declared.append(y); ctx.depth[y] = depth
        elif k == 'usingb':                             # §28.10: unmarked `using var y := x` over an owned x is rejected; over a borrow it pins the borrow
            y, x = s[1], s[2]
            read_check(x, G, site, ctx)
            st = G[x]
            if st[0] in ('own', 'disposed'):
                raise Reject('§28.10-unmarked-owned-into-using-var')
            G[y] = borrow_of(x, st, G); ctx.types[y] = ctx.types[x]; ctx.pins.add(y); declared.append(y); ctx.depth[y] = depth
        elif k == 'send':                               # K3.3: an `owned` argument — here Domain.Spawn (the K4.3 domain clause)
            x = s[1]
            consume_check(x, G, site, ctx, live, True)
            G[x] = ('moved',)
        elif k == 'asg':                                # K3.11 drop-on-overwrite at a binding; state match (§28.3.3)
            x = s[1]; st = G[x]
            if x in ctx.pins and RULES['A5_pin']:
                raise Reject('A5-reassign-using-var')
            if st[0] == 'brw':
                raise Reject('state-match')                 # a borrowed-state LHS takes no owned RHS
            if st[0] == 'own' and RULES['K5_freeze_overwrite'] and frozen(st[1], G, live):
                raise Reject('K5.2-frozen-overwrite')       # K5.2: fails (i), the reassignment is not keyword-visible
            ctx.asg_static[site] = st[0]; ctx.releases.append((order, x, st[0], loop))
            G[x] = ('own', frozenset({site}))               # moved/disposed/maybe LHS: owned-state, displaces nothing or per path
        elif k == 'asgb':                               # §28.3.3 state match: a borrow RHS needs a borrowed-state or adopting LHS
            x, y = s[1], s[2]
            read_check(y, G, site, ctx)
            if x in ctx.pins and RULES['A5_pin']:
                raise Reject('A5-reassign-using-var')
            if G[x][0] in ('own', 'disposed', 'maybe', 'moved'):
                raise Reject('state-match')                 # every owned-state LHS (rules 1–4) requires an owned RHS
            nb = borrow_of(y, G[y], G)
            escape_check(x, nb, ctx)                        # K2.3: the LHS may not outlive the borrow's sources
            G[x] = nb
        elif k == 'asgm':                               # K3.4 assignment form + K3.11 at the LHS
            x, y = s[1], s[2]
            if x in ctx.pins and RULES['A5_pin']:
                raise Reject('A5-reassign-using-var')
            sx = G[x]
            if sx[0] == 'brw':
                raise Reject('state-match')
            ids = consume_check(y, G, site, ctx, live, True)
            if sx[0] == 'own' and RULES['K5_freeze_overwrite'] and frozen(sx[1], G, live):
                raise Reject('K5.2-frozen-overwrite')
            ctx.asg_static[site] = sx[0]; ctx.releases.append((order, x, sx[0], loop))
            G[y] = ('moved',); G[x] = ('own', ids)
        elif k == 'disp':                               # K3.9: explicit disposal, site-announced (K4.2), unfrozen (K5.3)
            x = s[1]; st = G[x]
            if st[0] in ('moved', 'maybe', 'uninit'):
                if RULES['K1_3_moved_unreadable']:
                    raise Reject('K1.3-read-of-moved')
                ids = frozenset()
            elif st[0] == 'brw':
                raise Reject('§28.6-dispose-of-borrow')     # only owners dispose
            else:
                ids = st[1]
                if st[0] == 'disposed' and RULES['warn_use_after_dispose']:
                    ctx.warns.add((site, '§28.2-second-dispose'))   # a repeat call is a use, and stays a warning under K5.3-b
                if not RULES['K5_unfreeze_dispose'] and frozen(ids, G, live):
                    raise Reject('K5.3c-frozen-dispose')
            ctx.disp_sites[site] = dict(x=x, static=st[0], frozen=frozen(ids, G, live), self_live=(x in live),
                                        taint=bool(ids & ctx.taint), order=order, loop=loop)
            if st[0] == 'disposed':
                ctx.releases.append((order, x, st[0], loop))
            G['__killed'] = G['__killed'] | ids
            G[x] = ('disposed', ids)
        elif k == 'use':                                # a member access: K1.3 / §28.2 / K5.3-b
            read_check(s[1], G, site, ctx)
        elif k == 'fst':                                # K3.11 at a slot: drop-on-overwrite of the slot's value; K5.6: not frozen
            read_check(s[1], G, site, ctx)
        elif k == 'fstm':                               # K3.1: store into an owned field, marker-free; frozen (K5.1)
            x, y = s[1], s[2]
            read_check(x, G, site, ctx)
            consume_check(y, G, site, ctx, live, True)
            G[y] = ('moved',)
        elif k == 'fbrw':                               # K1.5 / K9.1: a storage read lends; its source is the holder
            y, x = s[1], s[2]
            read_check(x, G, site, ctx)
            G[y] = borrow_of(x, G[x], G); ctx.types[y] = TYPES_Q; declared.append(y); ctx.depth[y] = depth
        elif k == 'fmove':                              # K9.1 the replacing move: owned result, no intermediate state (K9.2)
            y, x = s[1], s[2]
            read_check(x, G, site, ctx)
            if frozen(ids_of(G[x]), G, live):           # a live borrow of the holder may alias the displaced value (§28.8.1: runtime-checked)
                ctx.taint.add(site)
            if RULES['freeze_linked_fmove'] and (ids_of(G[x]) & G['__linked']):
                raise Reject('owner-outlives-linked-child-moved-out')
            if RULES['linked_fmove_carries'] and (ids_of(G[x]) & G['__linked']):
                G['__carry'] = dict(G['__carry']); G['__carry'][y] = frozenset(ids_of(G[x]))
            G[y] = ('own', frozenset({site})); ctx.types[y] = TYPES_Q; declared.append(y); ctx.depth[y] = depth
        elif k == 'newk':                               # K4.3 field-held admission through a callee: `var x := new K(y)` where K stores y in a plain field
            x, y = s[1], s[2]
            read_check(y, G, site, ctx)
            srcs = ids_of(G[y]) if G[y][0] != 'brw' else G[y][1]
            G[x] = ('own', frozenset({site})); ctx.types[x] = TYPES_K; declared.append(x); ctx.depth[x] = depth
            if RULES['derived_retention']:
                G['__carry'] = dict(G['__carry']); G['__carry'][x] = frozenset(srcs)
        elif k == 'kuse':                               # a member access through the holder's stored borrow
            x = s[1]
            read_check(x, G, site, ctx)
            cs = carried(x, G)
            if cs and cs <= G['__killed']:              # 0205's holder rule under derived retention: the kill dominates the use
                raise Reject('0205-holder-use-after-dominating-kill')
        elif k == 'link':                               # the owned child's back-pointer to its owner (`x.f.Attach(x)`): admitted (parameter-sourced in the callee)
            x = s[1]
            read_check(x, G, site, ctx)
            G['__linked'] = G['__linked'] | ids_of(G[x])
        elif k == 'puse':                               # a member access through the child's back-pointer
            read_check(s[1], G, site, ctx)
            cs = carried(s[1], G)
            if cs and cs <= G['__killed']:              # the sharp owner-outlives rule: a moved-out child is a holder (0205's holder rule)
                raise Reject('0205-holder-use-after-dominating-kill')
        elif k in ('tuse', 'tsend'):                    # K8.1: an unbound temporary dies at the boundary / transfers (K3.3)
            pass
        elif k == 'tdisp':                              # K3.9 on an unbound temporary; the boundary reclaims only (§28.3.11)
            ctx.disp_sites[site] = dict(x=None, static='own', frozen=False, self_live=False, taint=False, order=order, loop=loop)
        elif k == 'tbrw':                               # K8.4: a borrow of a temporary escaping its boundary is rejected, never extended
            if RULES['K8_4_reject_escape']:
                raise Reject('K8.4-temporary-escape')
            G[s[1]] = ('brw', frozenset({site}), False, frozenset()); ctx.types[s[1]] = TYPES_Q; declared.append(s[1]); ctx.depth[s[1]] = depth
        elif k == 'throw':                              # K3.8: keyword-announced consume; unfrozen (K5.3); disposed after the catch arm (§31.1)
            x = s[1]
            ids = consume_check(x, G, site, ctx, live, False)
            G[x] = ('moved',)
            ctx.static[site] = dict(G)
            ctx.releases += [(order, v, G[v][0], loop) for v in G if not v.startswith('__')]   # the unwind is a release point of every in-scope binding (K3.15)
            if ctx.throw_stack:
                ctx.throw_stack[-1].append((dict(G), ids))
            G = None; break                             # K7.5: what follows is unreachable
        elif k == 'tthrow':
            ctx.static[site] = dict(G)
            ctx.releases += [(order, v, G[v][0], loop) for v in G if not v.startswith('__')]
            if ctx.throw_stack:
                ctx.throw_stack[-1].append((dict(G), frozenset({site})))
            G = None; break
        elif k == 'if':                                 # K7: the lattice meet at the join
            G1 = check_body(s[1], site + ('t',), dict(G), live, exc_live, ctx, depth=depth + 1)
            G2 = check_body(s[2], site + ('f',), dict(G), live, exc_live, ctx, depth=depth + 1)
            G = meet(G1, G2)
        elif k == 'block':                              # K3.14: scope end disposes the block's undischarged owned locals
            G = check_body(s[1], site + ('b',), dict(G), live, exc_live, ctx, depth=depth + 1)
        elif k == 'try':                                # K3.15: the handler's entry is the meet of every throw site; e borrows the exception (§31.1)
            S, e, H = s[1], s[2], s[3]
            lh = annotate(H, live, exc_live)[0]
            ctx.throw_stack.append([])
            G1 = check_body(S, site + ('s',), dict(G), live, lh, ctx, depth=depth + 1)
            thrown = ctx.throw_stack.pop()
            if not thrown:
                G = G1                                      # no throw site: the handler is dead code
            else:
                Gh = None; ids = frozenset()
                for Gt, tids in thrown:
                    Gr = {v: Gt[v] for v in G if not v.startswith('__')}; Gr['__killed'] = Gt['__killed']; Gr['__carry'] = Gt['__carry']; Gr['__linked'] = Gt['__linked']   # bindings the body declared are out of scope (unwound)
                    Gh = Gr if Gh is None else meet(Gh, Gr)
                    ids |= tids
                Gh[e] = ('brw', ids, ids <= Gh['__killed'], frozenset({e})); ctx.types[e] = TYPES_R   # §31.1: e borrows the in-flight exception; its scope is the arm
                G2 = check_body(H, site + ('h',), Gh, live, exc_live, ctx, pre_declared=(e,), depth=depth + 1)
                G = meet(G1, G2)
        elif k == 'while':                              # K7: back edges meet; K7.4 — a maybe at the head rejects any in-loop read
            ctx.loop_stack.append(site)
            head = G
            hl = loop_head_live(s[1], live, exc_live)
            for _ in range(8):
                Gend = check_body(s[1], site + ('w',), dict(head), hl, exc_live, ctx, depth=depth + 1)
                new = meet(head, Gend)
                if new == head:
                    break
                head = new
            else:
                raise Reject('loop-fixpoint')
            ctx.loop_stack.pop()
            G = head
        else:
            raise ValueError(f'unknown statement {s}')
        if G is None:
            break
    if G is not None:
        ctx.static[path] = {v: G[v] for v in declared}
        ctx.order += 1                                  # the scope end is a release site after the body's last statement
        for v in declared:
            ctx.releases.append((ctx.order, v, G[v][0], loop))
        G = {v: st for v, st in G.items() if v not in declared}
    return G

def meet(G1, G2):
    """K7 at a join; K7.5: an unreachable (bottom) edge absorbs."""
    if G1 is None: return G2
    if G2 is None: return G1
    G = {}
    for v in G1:
        if v == '__killed':
            G[v] = G1[v] & G2[v]                        # a kill dominates only where it is on every path
        elif v == '__carry':
            c = dict(G1[v])
            for b, sr in G2[v].items(): c[b] = c.get(b, frozenset()) | sr
            G[v] = c
        elif v == '__linked':
            G[v] = G1[v] | G2[v]
        else:
            G[v] = meet_cell(G1[v], G2[v])
    return G

def meet_cell(a, b):
    if HOOKS['meet']:
        return HOOKS['meet'](a, b)
    if a == b:
        return a
    ka, kb = a[0], b[0]
    if ka == kb == 'own':
        return ('own', a[1] | b[1])                     # K7.1
    if ka == kb == 'brw':
        return ('brw', a[1] | b[1], a[2] and b[2], a[3] | b[3])   # K7.2 / K2.2: union of the source sets
    if ka == kb == 'disposed':
        return ('disposed', a[1] | b[1])
    if {ka, kb} == {'own', 'brw'}:
        if RULES['K7_mixed_rejects']:
            raise Reject('K7.3-mixed-merge')            # K7.3
        return a if ka == 'brw' else b                  # the mutant: demote
    ids = ids_of(a) | ids_of(b)
    if 'own' in (ka, kb):
        return ('maybe', ids) if RULES['K7_maybe'] else ('own', ids)   # K7.4
    return ('maybe', ids)                               # moved ⊓ disposed, uninit ⊓ brw and the like: unreadable, per-path at scope end

# ----------------------------------------------------------------------------- the machine

class Thrown(Exception):
    def __init__(self, loc, site):
        self.loc = loc; self.site = site

class Halt(Exception):
    def __init__(self, outcome, info):
        self.outcome = outcome; self.info = info

class Run:
    """One execution. `choices` resolves the nondeterministic branches and loop counts in order;
    beyond the given prefix the first alternative is taken and the arity is recorded, so
    `run_all` can enumerate every path.  mode='retain': K4.1's flag persists for every disposed
    object (the quarantine abstraction).  mode='free': an explicit Dispose() at a site in
    `free_sites` reclaims the object in full (the #4602 free-in-full rule), after which any
    flag read of it is a UAF."""
    def __init__(self, ctx, choices=(), mode='retain', free_sites=frozenset()):
        self.ctx = ctx; self.choices = list(choices); self.pos = 0; self.arities = []
        self.mode = mode; self.free_sites = free_sites
        self.heap = {}        # loc -> [flag, slot loc|None, cls]   flag: live | disposed | freed | foreign
        self.env = {}         # var -> (loc|None, dtag)   dtag: own | brw | moved | disposed | uninit
        self.events = []      # ('disp', loc) | ('noop', loc, where) | ('release', loc, where) | ('mismatch', loc, what) | ...
        self.n = 0
        self.outcome = 'ok'; self.info = None

    # --- nondeterminism
    def choose(self, arity):
        self.arities.append(arity)
        c = self.choices[self.pos] if self.pos < len(self.choices) else 0
        self.pos += 1
        return c

    # --- heap
    def alloc(self, cls, borrow=None):
        slot = self.alloc(TYPES_Q) if cls == TYPES_R else None     # `new R(new Q())`: the argument first
        self.n += 1; self.heap[self.n] = ['live', slot if cls != TYPES_K else borrow, cls, None]; self.events.append(('new', self.n))
        return self.n

    def dispose(self, loc, where):
        """DISPOSE(l): first call sets the flag, emits disp(l) and cascades into the owned slot
        (K9); a later call is the idempotent no-op — a read of the flag (K4.1)."""
        st = self.heap[loc]
        if st[0] == 'live':
            st[0] = 'disposed'; self.events.append(('disp', loc))
            if st[1] is not None and st[2] == TYPES_R:        # K's slot is a BORROW: no cascade
                self.dispose(st[1], 'cascade')
        elif st[0] == 'disposed':
            self.events.append(('noop', loc, where))
        elif st[0] == 'freed':
            raise Halt('UAF', ('flag-read', where))
        else:
            raise Halt('RACE', ('dispose-of-foreign', where))

    def free(self, loc):
        """The free-in-full variant: the object's block, header word included, is gone."""
        st = self.heap[loc]
        st[0] = 'freed'; self.events.append(('free', loc))
        if st[1] is not None and st[2] == TYPES_R:
            self.free(st[1])

    def foreign(self, loc):
        st = self.heap[loc]
        st[0] = 'foreign'
        if st[1] is not None and st[2] == TYPES_R:
            self.foreign(st[1])

    def read(self, var):
        """A member access through a binding: K4.1's checked-mode test on the flag."""
        loc, tag = self.env[var]
        if tag in ('moved', 'uninit') or loc is None:
            raise Halt('STUCK', var)
        f = self.heap[loc][0]
        if f == 'disposed':
            raise Halt('FAULT', var)
        if f == 'freed':
            raise Halt('UAF', ('read', var))
        if f == 'foreign':
            raise Halt('RACE', var)
        return loc

    def through(self, target, what):
        """a member access through a stored borrow (a holder's field or a child's back-pointer): K4.1's flag test on the target."""
        if target is None:
            return None                                  # a null back-pointer: nothing to read (not a memory fault)
        f = self.heap[target][0]
        if f == 'disposed':
            raise Halt('FAULT', what)
        if f == 'freed':
            raise Halt('UAF', ('read', what))
        if f == 'foreign':
            raise Halt('RACE', what)
        return target

    def binding(self, var):
        loc, tag = self.env[var]
        if tag in ('moved', 'uninit') or loc is None:
            raise Halt('STUCK', var)
        if self.heap[loc][0] == 'foreign':
            raise Halt('RACE', var)
        return loc

    def release(self, loc, static, tag, where):
        """A holder's release point (§28.2's compiler-inserted free) for a binding whose STATIC
        state is `static` and whose runtime tag is `tag`: static own → dispose; static maybe →
        per path, the idempotent disposer (a flag read on the disposed path); static
        explicitly-disposed → the block is freed without a disposer call (K1's "scope-end
        machinery skips its disposal"); borrowed / moved / uninit → nothing."""
        if static == 'own':
            if tag in ('own', 'disposed'):
                self.dispose(loc, where)
            elif tag == 'moved':
                self.dispose(loc, where); self.events.append(('mismatch', loc, 'static-own-runtime-moved'))
            elif tag == 'brw':
                self.dispose(loc, where); self.events.append(('mismatch', loc, 'static-own-runtime-borrow'))
        elif static == 'maybe':
            if tag in ('own', 'disposed'):
                self.dispose(loc, where)
        elif static == 'disposed':
            if RULES['release_skips_disposed']:
                self.events.append(('release', loc, where))
            else:
                self.dispose(loc, where)                # the mutant: the disposer runs again at the release point

    def scope_end(self, declared, table, where):
        """K3.14 / K3.15: reverse establishment order, driven by the static classification."""
        for v in reversed(declared):
            loc, tag = self.env[v]
            if loc is None:
                continue
            st = table.get(v, (tag,))[0]
            self.release(loc, st, tag, where)

    # --- statements
    def body(self, prog, path, pre=()):
        declared = list(pre)
        try:
            for i, s in enumerate(prog):
                self.step(s, path + (i,), declared)
        except Thrown as t:
            if RULES['K3_15_edge_parity']:
                self.scope_end(declared, self.ctx.static.get(t.site, {}), 'unwind')
            raise
        self.scope_end(declared, self.ctx.static.get(path, {}), 'scope-end')

    def step(self, s, site, declared):
        k = s[0]; env = self.env; heap = self.heap
        if k in ('new', 'using'):
            env[s[1]] = (self.alloc(TYPES_R), 'own'); declared.append(s[1])
        elif k == 'decl':
            env[s[1]] = (None, 'uninit'); declared.append(s[1])
        elif k in ('brw', 'usingb'):
            loc = self.binding(s[2]); tag = env[s[2]][1]
            if not RULES['K3_13_read_lends'] and tag == 'own':
                env[s[2]] = (loc, 'moved'); env[s[1]] = (loc, 'own')
            else:
                env[s[1]] = (loc, 'brw')
            declared.append(s[1])
        elif k in ('move', 'usingm'):
            loc = self.binding(s[2])
            env[s[2]] = (loc, 'moved'); env[s[1]] = (loc, 'own'); declared.append(s[1])
        elif k == 'send':
            loc = self.binding(s[1])
            self.foreign(loc); env[s[1]] = (loc, 'moved')
        elif k == 'asg':
            old, tag = env[s[1]]
            static = self.ctx.asg_static.get(site, tag)
            if RULES['K9_5_rhs_first']:
                new = self.alloc(self.ctx.types.get(s[1], TYPES_R))
                if old is not None: self.release(old, static, tag, 'overwrite')
            else:
                if old is not None: self.release(old, static, tag, 'overwrite')
                new = self.alloc(self.ctx.types.get(s[1], TYPES_R))
            env[s[1]] = (new, 'own')
        elif k == 'asgb':
            loc = self.binding(s[2]); env[s[1]] = (loc, 'brw')
        elif k == 'asgm':
            old, tag = env[s[1]]
            loc = self.binding(s[2])
            if old is not None: self.release(old, self.ctx.asg_static.get(site, tag), tag, 'overwrite')
            env[s[2]] = (loc, 'moved'); env[s[1]] = (loc, 'own')
        elif k == 'disp':
            loc = self.binding(s[1])
            info = self.ctx.disp_sites.get(site, {})
            if self.mode == 'free' and info.get('static') == 'disposed':
                self.events.append(('elided', loc, 'repeat-dispose'))     # #4602: a dominated repeat call emits nothing
            else:
                self.dispose(loc, 'explicit')
                if self.mode == 'free' and site in self.free_sites and heap[loc][0] == 'disposed':
                    self.free(loc)
            env[s[1]] = (loc, 'disposed')
        elif k == 'use':
            self.read(s[1])
        elif k == 'fst':
            loc = self.read(s[1])
            if RULES['K9_5_rhs_first']:
                new = self.alloc(TYPES_Q); self.dispose(heap[loc][1], 'slot-overwrite')
            else:
                self.dispose(heap[loc][1], 'slot-overwrite'); new = self.alloc(TYPES_Q)
            heap[loc][1] = new
        elif k == 'fstm':
            loc = self.read(s[1]); yl = self.binding(s[2])
            self.dispose(heap[loc][1], 'slot-overwrite'); heap[loc][1] = yl
            env[s[2]] = (yl, 'moved')
        elif k == 'fbrw':
            loc = self.read(s[2]); env[s[1]] = (heap[loc][1], 'brw'); declared.append(s[1])
        elif k == 'fmove':
            loc = self.read(s[2]); new = self.alloc(TYPES_Q)        # the replacement first (K9.5); the exchange disposes nothing
            env[s[1]] = (heap[loc][1], 'own'); heap[loc][1] = new; declared.append(s[1])
        elif k == 'newk':
            yl = self.binding(s[2])
            env[s[1]] = (self.alloc(TYPES_K, borrow=yl), 'own'); declared.append(s[1])
        elif k == 'kuse':
            loc = self.read(s[1]); self.through(heap[loc][1], ('kuse', s[1]))
        elif k == 'link':
            loc = self.read(s[1]); heap[heap[loc][1]][3] = loc
        elif k == 'puse':
            loc = self.read(s[1]); self.through(heap[loc][3], ('puse', s[1]))
        elif k == 'tuse':
            t = self.alloc(TYPES_R); self.dispose(t, 'boundary')
        elif k == 'tdisp':
            t = self.alloc(TYPES_R); self.dispose(t, 'explicit')
            if self.mode == 'free' and site in self.free_sites:
                self.free(t)
            else:
                self.events.append(('release', t, 'boundary'))
        elif k == 'tsend':
            self.foreign(self.alloc(TYPES_R))
        elif k == 'tbrw':
            t = self.alloc(TYPES_R); env[s[1]] = (heap[t][1], 'brw'); declared.append(s[1])
            self.dispose(t, 'boundary')
        elif k == 'throw':
            loc = self.binding(s[1]); env[s[1]] = (loc, 'moved')
            raise Thrown(loc, site)
        elif k == 'tthrow':
            raise Thrown(self.alloc(TYPES_R), site)
        elif k == 'if':
            c = self.choose(2)
            self.body(s[1] if c == 0 else s[2], site + ('t' if c == 0 else 'f',))
        elif k == 'block':
            self.body(s[1], site + ('b',))
        elif k == 'try':
            try:
                self.body(s[1], site + ('s',))
            except Thrown as t:
                env[s[2]] = (t.loc, 'brw')
                try:
                    self.body(s[3], site + ('h',), pre=(s[2],))
                except Thrown:
                    self.dispose(t.loc, 'catch-done'); raise      # §31.1: a fresh throw leaving the arm disposes the caught one
                self.dispose(t.loc, 'catch-done')                  # E-CatchDone
        elif k == 'while':
            n = self.choose(3)
            for _ in range(n):
                self.body(s[1], site + ('w',))
        else:
            raise ValueError(f'unknown statement {s}')

    def execute(self, prog):
        try:
            self.body(prog, ())
        except Thrown as t:
            try:
                self.dispose(t.loc, 'termination')      # §31.1: disposed after the exception terminates the program
            except Halt as h:
                self.outcome = h.outcome; self.info = h.info
        except Halt as h:
            self.outcome = h.outcome; self.info = h.info
        return self

def run_all(prog, ctx, cap=256, **kw):
    """Every path: an odometer over the recorded arities of the choice points each run met."""
    runs = []; choices = []
    while True:
        r = Run(ctx, choices, **kw).execute(prog); runs.append(r)
        ar = r.arities
        ch = list(choices[:len(ar)]) + [0] * max(0, len(ar) - len(choices))
        i = len(ar) - 1
        while i >= 0 and ch[i] + 1 >= ar[i]:
            i -= 1
        if i < 0 or len(runs) >= cap:
            return runs
        choices = ch[:i] + [ch[i] + 1]

# ----------------------------------------------------------------------------- the theorems

def judge(runs):
    """T1: exactly one disp per allocation that stays in this domain, on every complete path,
    and the machinery never disposes what another holder owns.  T2/T3: no STUCK, no RACE (a
    FAULT is the backstop doing its job).  UAF: the free-in-full variant read freed memory."""
    bad = []
    for r in runs:
        if r.outcome in ('RACE', 'STUCK', 'UAF'):
            bad.append(r.outcome); continue
        if r.outcome == 'FAULT':
            continue
        disp = Counter(e[1] for e in r.events if e[0] == 'disp')
        for loc, st in r.heap.items():
            if st[0] == 'foreign':
                continue
            if disp[loc] != 1:
                bad.append('T1-leak' if disp[loc] == 0 else 'T1-double')
        if any(e[0] == 'mismatch' for e in r.events):
            bad.append('T1-static-mismatch')
    return bad

def trace_facts(runs):
    """Trace-level facts about an accepted program (the #4602 reading of the trace): which
    release sites read the flag of an already-disposed object (`noop`, by site kind) — the
    sites that force header retention — and the fault profile."""
    noops = Counter(e[2] for r in runs for e in r.events if e[0] == 'noop')
    outs = [r.outcome for r in runs]
    return noops, outs

def fault_profile(outs):
    if all(o == 'FAULT' for o in outs): return 'every'
    if any(o == 'FAULT' for o in outs): return 'some'
    return 'never'

# ----------------------------------------------------------------------------- retention (#4602)

RET_CONDS = ('borrow', 'self', 'slot', 'later')

def free_sites(ctx, conds):
    """The explicit-Dispose() sites the free-in-full rule admits under a set of conditions:
      borrow — no borrow of the disposed value is live after the call (K5's analysis; the ruling's condition 1)
      self   — the disposed binding itself is dead after the call (no §28.2-warned later use of it)
      slot   — the value never left a slot while a borrow of its holder was live (§28.8.1's runtime-checked alias)
      later  — no later release site of the same binding runs the idempotent disposer (static maybe-consumed) (condition 2)"""
    out = set()
    for site, d in ctx.disp_sites.items():
        if 'borrow' in conds and d['frozen']: continue
        if 'self' in conds and d['self_live']: continue
        if 'slot' in conds and d['taint']: continue
        if 'later' in conds and d['x'] is not None and any(
                x == d['x'] and kind == 'maybe' and (o > d['order'] or (lp is not None and lp == d['loop']))
                for o, x, kind, lp in ctx.releases):
            continue
        out.add(site)
    return frozenset(out)

# ----------------------------------------------------------------------------- enumeration

FEATURE_SETS = {
    'locals': {'locals'},
    'all': {'locals', 'asgm', 'pins', 'fields', 'temps', 'throw'},
    'trust': {'locals', 'fields', 'holders'},
}

def decl_type(decl, s):
    k = s[0]
    if k in ('new', 'decl', 'using'): return TYPES_R
    if k in ('brw', 'move', 'usingm', 'usingb'): return decl[s[2]]
    if k in ('fbrw', 'fmove', 'tbrw'): return TYPES_Q
    if k == 'newk': return TYPES_K
    return None

def atoms(vs, decl, feats, last, allow_decl=True):
    """Well-formed statements over `decl` (name -> type): reads name declared bindings,
    declarations name fresh ones; `throw` only as a body's last statement (no dead code)."""
    for x in vs:
        if x not in decl:
            if not allow_decl: continue
            if 'locals' in feats: yield ('new', x); yield ('decl', x)
            if 'pins' in feats: yield ('using', x)
            if 'temps' in feats: yield ('tbrw', x)
            for y in vs:
                if y in decl:
                    if 'locals' in feats: yield ('brw', x, y); yield ('move', x, y)
                    if 'pins' in feats: yield ('usingm', x, y); yield ('usingb', x, y)
                    if 'fields' in feats and decl[y] == TYPES_R: yield ('fbrw', x, y); yield ('fmove', x, y)
                    if 'holders' in feats and decl[y] != TYPES_K: yield ('newk', x, y)
        else:
            if 'locals' in feats and decl[x] == TYPES_K:
                yield ('disp', x); yield ('use', x)        # a holder: no reassignment form in the grammar; not sendable (it holds a borrow)
            elif 'locals' in feats:
                yield ('asg', x); yield ('disp', x); yield ('send', x); yield ('use', x)
            if 'holders' in feats and decl[x] == TYPES_K: yield ('kuse', x)
            if 'holders' in feats and decl[x] == TYPES_R: yield ('link', x)
            if 'holders' in feats and decl[x] == TYPES_Q: yield ('puse', x)
            if 'fields' in feats and decl[x] == TYPES_R: yield ('fst', x)
            if 'throw' in feats and last and decl[x] == TYPES_R: yield ('throw', x)
            for y in vs:
                if y != x and y in decl:
                    if 'locals' in feats and decl[x] == decl[y] and decl[x] != TYPES_K: yield ('asgb', x, y)
                    if 'asgm' in feats and decl[x] == decl[y] and decl[x] != TYPES_K: yield ('asgm', x, y)
                    if 'fields' in feats and decl[x] == TYPES_R and decl[y] == TYPES_Q: yield ('fstm', x, y)
    if 'temps' in feats:
        yield ('tuse',); yield ('tdisp',); yield ('tsend',)
    if 'throw' in feats and last:
        yield ('tthrow',)

def enum_straight(vs, n, decl, feats, allow_decl=True, allow_throw=True):
    """All well-formed straight-line bodies of length exactly n."""
    if n == 0:
        yield []; return
    for s in atoms(vs, decl, feats, last=(n == 1 and allow_throw), allow_decl=allow_decl):
        d2 = dict(decl)
        t = decl_type(decl, s)
        if t: d2[s[1]] = t
        for rest in enum_straight(vs, n - 1, d2, feats, allow_decl, allow_throw):
            yield [s] + rest

def enum_upto(vs, nmax, decl, feats, allow_decl=True, allow_throw=True, nmin=0):
    for n in range(nmin, nmax + 1):
        yield from enum_straight(vs, n, decl, feats, allow_decl, allow_throw)

def enum_programs(vs, maxlen, feats, with_if=False, with_try=False, with_while=False, body=2):
    """Deterministic, bounded: straight-line bodies of length 1..maxlen; with a construct:
    prefix (1..maxlen-2) ; construct ; suffix (0..1).  Arms and handlers hold ≤1 statement and
    declare nothing; try/while bodies hold 1..`body` statements and may declare."""
    for n in range(1, maxlen + 1):
        yield from enum_straight(vs, n, {}, feats)
    if not (with_if or with_try or with_while):
        return
    for pre in enum_upto(vs, max(1, maxlen - 2), {}, feats, allow_throw=False, nmin=1):
        d = {}
        for s in pre:
            t = decl_type(d, s)
            if t: d[s[1]] = t
        suffixes = list(enum_upto(vs, 1, d, feats))
        if with_if:
            arms = list(enum_upto(vs, 1, d, feats, allow_decl=False))
            for a1 in arms:
                for a2 in arms:
                    if a1 >= a2: continue
                    for suf in suffixes:
                        yield pre + [('if', a1, a2)] + suf
        if with_try:
            bodies = list(enum_upto(vs, body, d, feats, nmin=1))
            de = dict(d); de['e'] = TYPES_R
            handlers = list(enum_upto(vs + ['e'], 1, de, feats, allow_decl=False))
            for b in bodies:
                for h in handlers:
                    for suf in suffixes:
                        yield pre + [('try', b, 'e', h)] + suf
        if with_while:
            bodies = list(enum_upto(vs, body, d, feats, nmin=1))
            for b in bodies:
                for suf in suffixes:
                    yield pre + [('while', b)] + suf

# ----------------------------------------------------------------------------- sweeps

class Row:
    """An accepted program's record: warnings, oracle verdicts, a hash of its disposal traces
    (order included), the per-path outcomes and the release-site flag reads.  A rejected program
    is recorded as its verdict string only (memory: sweeps reach 10^6 programs)."""
    __slots__ = ('prog', 'verdict', 'warns', 'bad', 'trace', 'outs', 'noops')
    def __init__(self, prog, verdict, warns, bad=(), trace=0, outs=(), noops=None):
        self.prog = prog; self.verdict = verdict; self.warns = warns; self.bad = bad
        self.trace = trace; self.outs = outs; self.noops = noops or {}

def evaluate(prog):
    v, w, ctx = check(prog)
    if v != 'accept':
        return Row(prog, v, tuple(w)), ctx
    runs = run_all(prog, ctx)
    bad = sorted(set(judge(runs)))
    if bad == ['RACE'] and any(r.outcome == 'RACE' and isinstance(r.info, tuple) and r.info[0] in ('kuse', 'puse') for r in runs) and not any(r.outcome == 'RACE' and not (isinstance(r.info, tuple) and r.info[0] in ('kuse', 'puse')) for r in runs):
        bad = ['K4.3-trust-residual']
    elif bad == ['RACE'] and ctx.taint and any(s[0] in ('send',) for s in iter_stmts(prog)):
        # K4.3's recorded slot-kill-by-move clause (§28.3.9 Scope, clause 1; §28.8.1): a value the replacing
        # move took out of a slot while a borrow of the holder was live, then transferred to another domain —
        # the spec admits it as a recorded trust obligation, not as a checked rule; T3 holds only under it.
        bad = ['K4.3-trust-residual']
    trace = hash(tuple(tuple(e for e in r.events if e[0] in ('new', 'disp', 'noop', 'release', 'free')) + (r.outcome,) for r in runs))
    noops, outs = trace_facts(runs)
    return Row(prog, v, tuple(w), tuple(bad), trace, tuple(outs), dict(noops)), ctx

def sweep_iter(vs, maxlen, feats, with_if, with_try, with_while, body):
    for prog in enum_programs(vs, maxlen, feats, with_if, with_try, with_while, body):
        row, ctx = evaluate(prog)
        yield key(prog), row, ctx

def sweep(a, vs, feats, keep_ctx=False):
    """rows: key -> Row (accepted) | verdict string (rejected)."""
    rows = {}; ctxs = {}
    for k, row, ctx in sweep_iter(vs, a.len, feats, a.with_if, a.with_try, a.with_while, a.body):
        if row.verdict == 'accept':
            rows[k] = row
            if keep_ctx: ctxs[k] = ctx
        else:
            rows[k] = sys.intern(row.verdict)
    return rows, ctxs

def iter_stmts(prog):
    for s in prog:
        yield s
        for part in s[1:]:
            if isinstance(part, list):
                yield from iter_stmts(part)

def accepted(rows):
    return [r for r in rows.values() if isinstance(r, Row)]

def summarize(rows):
    st = Counter()
    for r in rows.values():
        st['programs'] += 1
        if isinstance(r, Row):
            st['accept'] += 1
            if r.warns: st['accept-with-warning'] += 1
            if any(w.startswith('§28.2') for w in r.warns): st['§28.2-warned'] += 1
            if any(s[0] == 'kuse' for s in iter_stmts(r.prog)): st['programs-with-kuse'] += 1
            if any(s[0] == 'puse' for s in iter_stmts(r.prog)): st['programs-with-puse'] += 1
            if r.bad == ('K4.3-trust-residual',): st['K4.3-trust-residual (T3 under the recorded clause)'] += 1
            elif r.bad: st['VIOLATIONS'] += 1
            if r.noops: st['flag-read-at-release'] += 1
            st['fault-' + fault_profile(r.outs)] += 1
        else:
            st[r] += 1
    return st

def minimal(rows_list, n=3):
    return sorted(rows_list, key=lambda r: (len(show(r.prog)), oneline(r.prog)))[:n]

def bounds_str(a):
    s = f'vars={a.vars} len={a.len} features={a.features}'
    s += ''.join(f' --{f}' for f in ('if', 'try', 'while') if getattr(a, 'with_' + f))
    if a.with_try or a.with_while: s += f' --body {a.body}'
    return s

def cmd_enumerate(a, vs, feats):
    t0 = time.time()
    rows, _ = sweep(a, vs, feats)
    st = summarize(rows)
    print(f'bounds: {bounds_str(a)}   ({time.time() - t0:.1f} s)')
    print('baseline kernel:')
    for k2, v in sorted(st.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f'  {v:8d}  {k2}')
    viol = [r for r in accepted(rows) if r.bad and r.bad != ('K4.3-trust-residual',)]
    for r in minimal(viol):
        print('VIOLATION', r.bad); print('\n'.join(show(r.prog, '    ')))
    resid = [r for r in accepted(rows) if r.bad == ('K4.3-trust-residual',)]
    if resid:
        print('K4.3 trust residual (T3 fails without the recorded slot-kill-by-move clause):', len(resid), 'programs, e.g.', oneline(minimal(resid, 1)[0].prog))
    noop_kinds = Counter()
    for r in accepted(rows):
        for kind in r.noops: noop_kinds[kind] += 1
    print('programs with a flag read of an already-disposed object at a release site, by site kind:', dict(noop_kinds))
    print('T1/T2/T3 hold on every accepted program' if not viol else 'SPEC/MODEL FINDING: see above')

# ----------------------------------------------------------------------------- mutations

MUTANTS = {
    'K5.1 deleted (transfers unfrozen) — the pre-v0.180 language': dict(K5_freeze_transfers=False),
    'K5.2 deleted (drop-on-overwrite unfrozen)': dict(K5_freeze_overwrite=False),
    'K5.3-c: explicit Dispose() frozen under any live borrow': dict(K5_unfreeze_dispose=False),
    'K5.3-a: dominated use of a killed borrow / same-variable use — warning only': dict(K5_3b_borrow='warn'),
    'K5.3-b: dominated use of a killed borrow and same-variable dominated use — errors': dict(K5_3b_borrow='error', K5_3b_samevar='error'),
    'K5.3-b (borrow half only)': dict(K5_3b_borrow='error'),
    'K5.3-b (same-variable half only)': dict(K5_3b_samevar='error'),
    'K7.3 weakened (owned ⊓ borrowed demotes to borrowed)': dict(K7_mixed_rejects=False),
    'K7.4 weakened (owned ⊓ moved = owned)': dict(K7_maybe=False),
    'K1.3 deleted (moved-from bindings readable)': dict(K1_3_moved_unreadable=False),
    'K3.13 flipped (an unmarked read consumes)': dict(K3_13_read_lends=False),
    'release sites of an explicitly-disposed binding run the disposer again (K3.9 post-state ignored)': dict(release_skips_disposed=False),
    '§28.2 use-after-dispose warning dropped': dict(warn_use_after_dispose=False),
    'K8.4 deleted (a borrow of a temporary may escape its boundary)': dict(K8_4_reject_escape=False),
    'A5 deleted (a using var may be reassigned or moved out)': dict(A5_pin=False),
    'K3.15 deleted (no implicit disposal on the exception edge)': dict(K3_15_edge_parity=False),
    'K9.5 flipped (the displaced value is disposed before the RHS is evaluated)': dict(K9_5_rhs_first=False),
    'derived retention (0132): a holder minted over a local carries it — K5 freeze + 0205 holder rule': dict(derived_retention=True),
    'owner-outlives: a linked child may not be moved out of its owner (fmove frozen)': dict(freeze_linked_fmove=True),
    'owner-outlives, sharp: a linked child moved out carries its owner (derived retention at the move)': dict(linked_fmove_carries=True),
    'both: derived retention + sharp owner-outlives': dict(derived_retention=True, linked_fmove_carries=True),
}

def load_definition(path):
    """A user-defined mutation: a Python file setting NAME, RULES_OVERRIDE (dict) and/or
    HOOKS_OVERRIDE (dict of callables for `frozen(ids, G, live)` / `meet(a, b)`)."""
    spec = importlib.util.spec_from_file_location('bz_mutation', path)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return getattr(mod, 'NAME', path), getattr(mod, 'RULES_OVERRIDE', {}), getattr(mod, 'HOOKS_OVERRIDE', {})

class mutated:
    def __init__(self, flips, hooks=None):
        self.flips = flips; self.hooks = hooks or {}
    def __enter__(self):
        self.saved = dict(RULES); self.saved_hooks = dict(HOOKS)
        RULES.update(self.flips); HOOKS.update(self.hooks)
    def __exit__(self, *a):
        RULES.update(self.saved); HOOKS.update(self.saved_hooks)

def diff(base, a, vs, feats, flips, hooks=None):
    """What a mutation changes, program by program: the mutant sweep is streamed against the
    baseline, only the differences are kept."""
    d = dict(new_acc=[], new_rej=[], newly_unsound=[], unsound_all=[], newly_warned=[], newly_unwarned=[], trace_changed=[],
             stats=Counter())
    with mutated(flips, hooks):
        for k, r, _ in sweep_iter(vs, a.len, feats, a.with_if, a.with_try, a.with_while, a.body):
            b = base[k]
            d['stats']['programs'] += 1
            if r.verdict == 'accept':
                d['stats']['accept'] += 1
                d['stats']['fault-' + fault_profile(r.outs)] += 1
                if r.bad: d['unsound_all'].append(r)
                if not isinstance(b, Row):
                    d['new_acc'].append(r)
                    if r.bad: d['newly_unsound'].append(r)
                else:
                    if r.bad and not b.bad: d['newly_unsound'].append(r)
                    if r.trace != b.trace: d['trace_changed'].append(r)
                    if r.warns and not b.warns: d['newly_warned'].append(r)
                    if b.warns and not r.warns: d['newly_unwarned'].append(r)
            elif isinstance(b, Row):
                d['new_rej'].append(b)
    return d

def profile_counts(rows_list):
    c = Counter(fault_profile(r.outs) for r in rows_list)
    return c['every'], c['some'], c['never']

def witness(rows_list):
    if not rows_list: return ''
    r = minimal(rows_list, 1)[0]
    extra = ''
    if r.bad: extra = ' → ' + ','.join(r.bad)
    elif r.verdict == 'accept' and r.outs: extra = f' (runs: {",".join(r.outs)})'
    return oneline(r.prog) + extra

def cmd_mutate(a, vs, feats):
    t0 = time.time()
    base, _ = sweep(a, vs, feats)
    bs = summarize(base)
    assert not bs['VIOLATIONS'], 'the baseline must be sound before mutating'
    print(f'bounds: {bounds_str(a)}')
    print(f'baseline: {bs["programs"]} programs, {bs["accept"]} accepted ({bs["accept-with-warning"]} with warnings, '
          f'{bs["§28.2-warned"]} §28.2-warned; fault profile every/some/never = {bs["fault-every"]}/{bs["fault-some"]}/{bs["fault-never"]}), '
          f'0 violations  ({time.time() - t0:.1f} s)\n')
    print('| mutation | verdicts changed | newly accepted | of which unsound | newly rejected (every/some/never fault) | unsound among all accepted | warnings / trace only | witness |')
    print('|---|---|---|---|---|---|---|---|')
    for name, flips in MUTANTS.items():
        d = diff(base, a, vs, feats, flips)
        changed = len(d['new_acc']) + len(d['new_rej'])
        e, s_, n = profile_counts(d['new_rej'])
        w = witness(d['newly_unsound']) or witness(d['unsound_all'])
        if not w and d['new_rej']:
            w = 'newly rejected: ' + witness(d['new_rej'])
        if not w and d['new_acc']:
            w = witness(d['new_acc']) + ' (sound: the flag backstops it)'
        side = []
        if d['newly_warned'] or d['newly_unwarned']:
            side.append(f'warned +{len(d["newly_warned"])}/−{len(d["newly_unwarned"])}')
        if d['trace_changed']:
            side.append(f'trace changed in {len(d["trace_changed"])}')
            if not w: w = 'trace: ' + witness(d['trace_changed'])
        print(f'| {name} | {changed} | {len(d["new_acc"])} | {len(d["newly_unsound"])} | {len(d["new_rej"])} ({e}/{s_}/{n}) | '
              f'{len(d["unsound_all"])} | {"; ".join(side)} | {w} |')

def cmd_check(a, vs, feats):
    if a.define:
        name, flips, hooks = load_definition(a.define)
    else:
        if a.mutation not in MUTANTS:
            near = [m for m in MUTANTS if a.mutation.lower() in m.lower()]
            if len(near) != 1:
                sys.exit(f'unknown mutation {a.mutation!r}; candidates: {near or list(MUTANTS)}')
            a.mutation = near[0]
        name, flips, hooks = a.mutation, MUTANTS[a.mutation], {}
    t0 = time.time()
    base, _ = sweep(a, vs, feats)
    bs = summarize(base)
    d = diff(base, a, vs, feats, flips, hooks)
    ms = d['stats']
    print(f'# Rule change: {name}')
    print(f'rules: {flips}' + (f'  hooks: {sorted(hooks)}' if hooks else ''))
    print(f'bounds: {bounds_str(a)}   ({time.time() - t0:.1f} s)\n')
    def block(title, rows_list, n=3, runs=True):
        print(f'## {title}: {len(rows_list)}')
        for r in minimal(rows_list, n):
            print('    ' + oneline(r.prog))
            if runs and r.verdict == 'accept':
                print(f'        runs: {",".join(r.outs)}' + (f'  bad: {",".join(r.bad)}' if r.bad else '') + (f'  warnings: {",".join(r.warns)}' if r.warns else ''))
    block('Newly accepted', d['new_acc'])
    block('Newly rejected', d['new_rej'])
    if d['new_rej']:
        e, s_, n = profile_counts(d['new_rej'])
        print(f'    fault profile under the baseline: {e} fault on every execution, {s_} on some execution, {n} never fault (pure over-rejection)')
        for label, sel in (('every', 'every'), ('some', 'some'), ('never', 'never')):
            w = [r for r in d['new_rej'] if fault_profile(r.outs) == sel]
            if w: print(f'    witness ({label}): ' + oneline(minimal(w, 1)[0].prog))
        warned = [r for r in d['new_rej'] if any(x.startswith('§28.2') for x in r.warns)]
        print(f'    of the newly rejected, {len(warned)} carried a §28.2 warning at the baseline and {len(d["new_rej"]) - len(warned)} were silent')
    block('Newly unsound (accepted by the mutant, violating T1/T2/T3)', d['newly_unsound'])
    block('Newly warned (verdict unchanged)', d['newly_warned'], runs=False)
    block('Newly un-warned (verdict unchanged)', d['newly_unwarned'], runs=False)
    block('Trace changed (verdict unchanged)', d['trace_changed'], runs=False)
    print()
    print('## Evaluation (paste-ready)')
    print(f'Model: bzcore L1 (spec v0.345), bounds {bounds_str(a)}: {bs["programs"]} programs, '
          f'{bs["accept"]} accepted at the baseline ({bs["§28.2-warned"]} with a §28.2 warning), 0 violations.')
    print(f'Under "{name}": {ms["accept"]} accepted; {len(d["new_acc"])} newly accepted ({len(d["newly_unsound"])} unsound), '
          f'{len(d["new_rej"])} newly rejected, {len(d["unsound_all"])} unsound among all accepted; '
          f'{len(d["newly_warned"])} newly warned, {len(d["newly_unwarned"])} newly un-warned; trace changed in {len(d["trace_changed"])}.')
    if d['new_rej']:
        e, s_, n = profile_counts(d['new_rej'])
        print(f'Of the newly rejected programs, {e} fault on every execution, {s_} on some execution and {n} never fault.')
    print(f'Run-time residual: at the baseline {bs["fault-every"]} accepted programs fault on every execution and {bs["fault-some"]} on some; '
          f'under the change {ms["fault-every"]} and {ms["fault-some"]} (checked-mode DisposedAccessException remains the backstop for these).')
    if d['newly_unsound']:
        print(f'Witness of unsoundness: {witness(d["newly_unsound"])}')
    elif d['new_rej']:
        print(f'Witness of a newly rejected program: {witness(d["new_rej"])}')

# ----------------------------------------------------------------------------- retention (#4602)

RET_FLAVORS = [
    ('free everywhere (no condition)', ()),
    ('condition 1 as worded: no live borrow of the value', ('borrow',)),
    ('condition 2 only: no later maybe-consumed release of the binding', ('later',)),
    ('the ruling: conditions 1 and 2 as worded', ('borrow', 'later')),
    ('1 + 2 + the binding itself dead after the call', ('borrow', 'self', 'later')),
    ('1 + 2 + binding dead + slot-displaced values retained', ('borrow', 'self', 'slot', 'later')),
    ('all but condition 1', ('self', 'slot', 'later')),
    ('all but condition 2', ('borrow', 'self', 'slot')),
]

def cmd_retention(a, vs, feats):
    """The #4602 trace oracle: for every accepted program with an explicit Dispose() site, run the
    free-in-full heap under each flavor of the rule's conditions and count the programs that read
    freed memory (expected: 0 only when the conditions suffice)."""
    t0 = time.time()
    st = Counter(); per = {title: dict(sites=0, uaf=[], readers=Counter()) for title, _ in RET_FLAVORS}
    for k, row, ctx in sweep_iter(vs, a.len, feats, a.with_if, a.with_try, a.with_while, a.body):
        st['programs'] += 1
        if row.verdict != 'accept':
            continue
        st['accept'] += 1
        assert not row.bad or row.bad == ('K4.3-trust-residual',), ('the baseline must be sound', oneline(row.prog))
        if row.noops: st['flag-read-at-release'] += 1
        if not ctx.disp_sites:
            continue
        st['with-explicit-dispose'] += 1
        for title, conds in RET_FLAVORS:
            fs = free_sites(ctx, conds); p = per[title]; p['sites'] += len(fs)
            if not fs: continue
            runs = run_all(row.prog, ctx, mode='free', free_sites=fs)
            hit = [r for r in runs if r.outcome == 'UAF']
            if hit:
                if len(p['uaf']) < 50: p['uaf'].append(row)
                p['n'] = p.get('n', 0) + 1
                for r in hit: p['readers'][r.info[1] if r.info[0] == 'read' else r.info[1]] += 1
    print(f'bounds: {bounds_str(a)}   ({time.time() - t0:.1f} s)')
    print(f'baseline: {st["programs"]} programs, {st["accept"]} accepted, {st["with-explicit-dispose"]} with an explicit Dispose() site; '
          f'{st["flag-read-at-release"]} read the flag of an already-disposed object at a release site under the retaining heap\n')
    print('| free-in-full rule | sites freed in full | accepted programs reading freed memory | by reader (variable or site kind) | witness |')
    print('|---|---|---|---|---|')
    for title, conds in RET_FLAVORS:
        p = per[title]
        w = oneline(minimal(p['uaf'], 1)[0].prog) if p['uaf'] else ''
        print(f'| {title} | {p["sites"]} | {p.get("n", 0)} | {dict(p["readers"])} | {w} |')
    print('\nreaders: a variable name = a member access through it; overwrite / scope-end / unwind = a release site running the idempotent disposer; cascade = a holder\'s teardown; explicit = a repeated Dispose()')

# ----------------------------------------------------------------------------- selftest

def X(*lines):
    return list(lines)

SELFTEST = [
    # --- the eight worked shapes of the locals model, re-run on L1
    ('v0.180 probe (rationale §5.1): borrow, transfer to a domain, use the borrow',
     [('new', 'x'), ('brw', 'b', 'x'), ('send', 'x'), ('use', 'b')], 'reject:K5.1-frozen-transfer', None, None),
    ('0047 §5.3 example: var b := x; var y := move x; use b',
     [('new', 'x'), ('brw', 'b', 'x'), ('move', 'y', 'x'), ('use', 'b')], 'reject:K5.1-frozen-transfer', None, None),
    ('K5.3: explicit Dispose() under a live borrow is admitted; the later use faults deterministically',
     [('new', 'x'), ('brw', 'b', 'x'), ('disp', 'x'), ('use', 'b')], 'accept', ['FAULT'], None),
    ('K5.2 (§28.7 Overwrite while borrowed): drop-on-overwrite under a live borrow is frozen',
     [('new', 'x'), ('brw', 'b', 'x'), ('asg', 'x'), ('use', 'b')], 'reject:K5.2-frozen-overwrite', None, None),
    ('K7.3 (§28.3.3 mixed merge): owned on one arm, borrowed on the other, rejected at the join',
     [('decl', 'x'), ('new', 'a'), ('if', [('asg', 'x')], [('asgb', 'x', 'a')]), ('use', 'x')], 'reject:K7.3-mixed-merge', None, None),
    ('§28.3.3 Pattern 1: equalize to borrowed',
     [('decl', 'x'), ('new', 'a'), ('new', 'b'), ('if', [('asgb', 'x', 'a')], [('asgb', 'x', 'b')]), ('use', 'x')], 'accept', ['ok', 'ok'], None),
    ('K7.4: a conditional kill joins to maybe-consumed; the later read is rejected',
     [('new', 'a'), ('if', [('disp', 'a')], []), ('use', 'a')], 'reject:K1.3-read-of-moved', None, None),
    ('K3.14 affinity: an undischarged owned local is disposed at scope end',
     [('new', 'a'), ('block', [('new', 'b'), ('use', 'b')]), ('use', 'a')], 'accept', ['ok'],
     [('disp', 4), ('disp', 3), ('disp', 2), ('disp', 1)]),      # b (R#4 then its Q#3) at the inner end, then a
    ('§28.2: straight-line use after an explicit kill is a warning, and faults in checked mode',
     [('new', 'a'), ('disp', 'a'), ('use', 'a')], 'accept', ['FAULT'], None),
    # --- L1 constructs
    ('§28.2 letter: a consume (move) of an explicitly-disposed binding is the same warning, not an error',
     [('new', 'a'), ('disp', 'a'), ('move', 'y', 'a')], 'accept', ['ok'], [('disp', 2), ('disp', 1), ('noop', 2, 'scope-end')]),
    ('§28.2 / #4602: a repeated Dispose() is a warning and a no-op flag read at run time',
     [('new', 'a'), ('disp', 'a'), ('disp', 'a')], 'accept', ['ok'], [('disp', 2), ('disp', 1), ('noop', 2, 'explicit'), ('release', 2, 'scope-end')]),
    ('K1 explicitly-disposed + §28.3.3 rule 3: scope end and an overwrite free the block without a disposer call',
     [('new', 'a'), ('disp', 'a'), ('asg', 'a')], 'accept', ['ok'], [('disp', 2), ('disp', 1), ('release', 2, 'overwrite'), ('disp', 4), ('disp', 3)]),
    ('K7.4 at scope end: a maybe-consumed binding is disposed per path; the disposed path reads the flag',
     [('new', 'a'), ('if', [('disp', 'a')], [])], 'accept', ['ok', 'ok'], None),
    ('A5: a using var may not be moved out',
     [('using', 'a'), ('move', 'y', 'a')], 'reject:A5-move-out-of-using-var', None, None),
    ('A5: a using var may not be reassigned',
     [('using', 'a'), ('asg', 'a')], 'reject:A5-reassign-using-var', None, None),
    ('§28.10: unmarked `using var y := x` over an owned x is rejected',
     [('new', 'a'), ('usingb', 'y', 'a')], 'reject:§28.10-unmarked-owned-into-using-var', None, None),
    ('§28.10: over a borrow it is a borrow-pin that keeps the source frozen while live',
     [('new', 'a'), ('brw', 'b', 'a'), ('usingb', 'p', 'b'), ('send', 'a'), ('use', 'p')], 'reject:K5.1-frozen-transfer', None, None),
    ('§28.10 adoption: using var y := move x transfers into the pin; disposed at scope end',
     [('new', 'a'), ('usingm', 'y', 'a')], 'accept', ['ok'], [('disp', 2), ('disp', 1)]),
    ('K9.1: a storage read lends; its source is the holder, so a transfer of the holder is frozen (K5.1)',
     [('new', 'a'), ('fbrw', 'b', 'a'), ('send', 'a'), ('use', 'b')], 'reject:K5.1-frozen-transfer', None, None),
    ('K5.6: a slot overwrite is not frozen; the stale borrow faults under K4.1',
     [('new', 'a'), ('fbrw', 'b', 'a'), ('fst', 'a'), ('use', 'b')], 'accept', ['FAULT'], None),
    ('K9.1/K9.5: the replacing move yields the displaced value owned and disposes nothing',
     [('new', 'a'), ('fmove', 'q', 'a'), ('use', 'q')], 'accept', ['ok'], [('disp', 1), ('disp', 2), ('disp', 3)]),   # q=Q#1 first (reverse establishment), then a=R#2 cascading into the replacement Q#3
    ('K3.1: a store into an owned field consumes the binding; frozen under a live borrow',
     [('new', 'a'), ('fmove', 'q', 'a'), ('brw', 'b', 'q'), ('fstm', 'a', 'q'), ('use', 'b')], 'reject:K5.1-frozen-transfer', None, None),
    ('K3.1: the store displaces the slot value (drop-on-overwrite at a slot) and the stored value is cascaded later',
     [('new', 'a'), ('fmove', 'q', 'a'), ('fstm', 'a', 'q')], 'accept', ['ok'], [('disp', 3), ('disp', 2), ('disp', 1)]),
    ('K8.1: an unbound temporary is disposed at the boundary, exactly once',
     [('tuse',)], 'accept', ['ok'], [('disp', 2), ('disp', 1)]),
    ('K3.9 on a temporary: the boundary reclaims only (no second call)',
     [('tdisp',)], 'accept', ['ok'], [('disp', 2), ('disp', 1), ('release', 2, 'boundary')]),
    ('K8.4: a borrow of a temporary may not escape its boundary',
     [('tbrw', 'b'), ('use', 'b')], 'reject:K8.4-temporary-escape', None, None),
    ('K3.8: throw consumes; §31.1: the exception is disposed after the catch arm; K3.15: the body\'s local is unwound first',
     [('new', 'a'), ('try', [('new', 'b'), ('throw', 'a')], 'e', [('use', 'e')])], 'accept', ['ok'],
     [('disp', 4), ('disp', 3), ('disp', 2), ('disp', 1)]),       # b unwound (R#4,Q#3) before the arm; then a (R#2,Q#1) after the arm
    ('K5.3: throw is unfrozen; a borrow surviving it reads a live object in the arm and faults after it',
     [('new', 'a'), ('brw', 'b', 'a'), ('try', [('throw', 'a')], 'e', [('use', 'b')]), ('use', 'b')], 'accept', ['FAULT'], None),
    ('K7 at the try join: owned on the normal edge, moved on the throwing edge → maybe-consumed',
     [('new', 'a'), ('try', [('if', [('throw', 'a')], [])], 'e', []), ('use', 'a')], 'reject:K1.3-read-of-moved', None, None),
    ('K3.15 edge parity: an uncaught throw unwinds the scope, then the exception is disposed at termination',
     [('new', 'a'), ('new', 'b'), ('throw', 'a')], 'accept', ['ok'], [('disp', 4), ('disp', 3), ('disp', 2), ('disp', 1)]),
    ('K7.4 at a back edge: a kill inside a loop makes the binding maybe-consumed at the head; the in-loop use is rejected',
     [('new', 'a'), ('while', [('use', 'a'), ('disp', 'a')])], 'reject:K1.3-read-of-moved', None, None),
    ('§28.3.3 loop-carried spelling: consume then reassign at the body\'s end stays owned around the back edge',
     [('new', 'a'), ('while', [('send', 'a'), ('asg', 'a')]), ('use', 'a')], 'accept', ['ok', 'ok', 'ok'], None),
    ('K2.3 at a local: a borrow of an inner-scope local may not reach an outer binding',
     [('decl', 'c'), ('block', [('new', 'b'), ('asgb', 'c', 'b')]), ('use', 'c')], 'reject:K2.3-borrow-outlives-source', None, None),
    ('§31.1 / K2.3: the catch binding\'s borrow may not leave the arm',
     [('new', 'a'), ('decl', 'c'), ('try', [('throw', 'a')], 'e', [('asgb', 'c', 'e')]), ('use', 'c')], 'reject:K2.3-borrow-outlives-source', None, None),
    ('K2.3: a borrow of an outer local may be stored into an inner binding',
     [('new', 'a'), ('block', [('decl', 'c'), ('asgb', 'c', 'a'), ('use', 'c')])], 'accept', ['ok'], None),
    ('K5.3-b residual: a borrow across a back edge is stale only on the second iteration (stays run-time)',
     [('new', 'a'), ('brw', 'b', 'a'), ('while', [('use', 'b'), ('disp', 'a'), ('asg', 'a')])], 'accept', ['ok', 'ok', 'FAULT'], None),
]

def cmd_selftest():
    fails = 0
    for title, prog, expect, outs, trace in SELFTEST:
        v, w, ctx = check(prog)
        ok = (v == expect)
        line = f'--- {title}\n    ' + '\n    '.join(show(prog)) + f'\n    verdict: {v}  warnings: {w}  (expected {expect})'
        got_outs = got_trace = None
        if v == 'accept':
            runs = run_all(prog, ctx)
            bad = judge(runs)
            got_outs = [r.outcome for r in runs]
            got_trace = [e for e in runs[0].events if e[0] in ('disp', 'noop', 'release')]
            line += f'\n    runs: {got_outs}  bad: {sorted(set(bad))}\n    trace[0]: {got_trace}'
            if bad: ok = False
            if outs is not None and got_outs != outs: ok = False
            if trace is not None and got_trace != trace: ok = False
        print(line + ('  OK' if ok else f'  MISMATCH (expected runs {outs}, trace {trace})'))
        if not ok: fails += 1
    # the K5.3-a/b mutations on the worked shapes
    with mutated(dict(K5_3b_borrow='error', K5_3b_samevar='error')):
        for prog, expect in [
            ([('new', 'x'), ('brw', 'b', 'x'), ('disp', 'x'), ('use', 'b')], 'reject:K5.3b-use-of-killed-borrow'),
            ([('new', 'a'), ('disp', 'a'), ('use', 'a')], 'reject:K5.3b-same-variable-dominated-use'),
            ([('new', 'a'), ('disp', 'a'), ('disp', 'a')], 'accept'),
            ([('new', 'x'), ('brw', 'b', 'x'), ('if', [('disp', 'x')], []), ('use', 'b')], 'accept'),
            ([('new', 'x'), ('disp', 'x'), ('brw', 'b', 'x')], 'reject:K5.3b-same-variable-dominated-use'),
            ([('new', 'a'), ('brw', 'b', 'a'), ('while', [('use', 'b'), ('disp', 'a'), ('asg', 'a')])], 'accept'),
        ]:
            v, w, _ = check(prog)
            ok = v == expect
            print(f'--- K5.3-b: {oneline(prog)}\n    verdict: {v} warnings: {w} (expected {expect})  ' + ('OK' if ok else 'MISMATCH'))
            if not ok: fails += 1
    print(f'\nselftest: {len(SELFTEST) + 6 - fails} passed, {fails} failed')
    return fails

# ----------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['selftest', 'enumerate', 'mutate', 'check', 'retention', 'mutations'])
    ap.add_argument('--vars', type=int, default=3)
    ap.add_argument('--len', type=int, default=4)
    ap.add_argument('--features', default='all', help='all | locals | a comma list of locals,asgm,pins,fields,temps,throw')
    ap.add_argument('--if', dest='with_if', action='store_true')
    ap.add_argument('--try', dest='with_try', action='store_true')
    ap.add_argument('--while', dest='with_while', action='store_true')
    ap.add_argument('--body', type=int, default=2, help='max statements in a try or while body (default 2)')
    ap.add_argument('--mutation', help='a name (or unique substring) from the mutation table')
    ap.add_argument('--define', help='a Python file defining a new mutation (NAME, RULES_OVERRIDE, HOOKS_OVERRIDE)')
    a = ap.parse_args()
    vs = ['a', 'b', 'c', 'd'][:a.vars]
    feats = FEATURE_SETS.get(a.features) or set(a.features.split(','))
    if a.cmd == 'selftest':
        sys.exit(1 if cmd_selftest() else 0)
    elif a.cmd == 'mutations':
        for m in MUTANTS: print(m)
    elif a.cmd == 'enumerate':
        cmd_enumerate(a, vs, feats)
    elif a.cmd == 'mutate':
        cmd_mutate(a, vs, feats)
    elif a.cmd == 'check':
        if not (a.mutation or a.define): sys.exit('check needs --mutation NAME or --define FILE')
        cmd_check(a, vs, feats)
    elif a.cmd == 'retention':
        cmd_retention(a, vs, feats)

if __name__ == '__main__':
    main()
