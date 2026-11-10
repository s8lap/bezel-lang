#!/usr/bin/env python3
"""bzcore_0212 — proposal 0212's model: a COPY of ../../model/bzcore_static.py (the static-ownership-tracking study's
model), extended with three statements, two rule knobs and two machine variants. Nothing in the originals is edited.

  Added statements (feature set `p0212` = `static` + mint2 + erase):
    newk2 x y z   var x := new K2(y, z);    a constructor with TWO plain arguments that stores only the first: the mint
                                            rule (0212 R2) carries the sources of BOTH, the machine stores y alone
    erase y x     var y: Object := move x;  an OWNED conversion of a holder to a type that announces no borrow (what the
                                            caller of `Hide<T>(owned x: T): owned Object` sees): y carries nothing
    euse y        y.ToString();             a member access through the erased holder's stored borrow (its override reads it)

  Added rule knobs:
    erasure_rule      0212 R5: an owned value whose source set is not empty may not be converted, at an owning position,
                      to a type that is not borrow-bearing: `erase y x` is rejected when x carries a source
    mint_stored_only  the oracle "A-explicit": a mint carries only the arguments the constructor really stores
                      (not a proposed rule: it measures the over-approximation of the signature-read mint)
  Rule completed (0212 R2 clause 2): `fstm x y` — an OWNED argument that carries sources brings them to the receiver:
    x gains y's carry, checked as an assignment to x's binding (K2.3; the declared-before test where a release reads).

  Added machine variants (not rules):
    --qdtor           the child class Q has a destructor that reads its back-pointer (set by `link`): a linked child's
                      release — in its owner's cascade, at a slot overwrite, or after being moved out — reads its owner
    --r40-teardown    the disposed flag is set when Dispose() BEGINS (what r40 does, probes d01-d03, d11); the default is
                      spec §28.12 (v0.355, #7506): an instance is logically disposed once its disposal has COMPLETED, so
                      the cascade's own accesses, a child's read through a back-reference included, run on a live instance

  Release (0212 R3), under --qdtor: a release that may run user code is a use at EVERY release point — the scope end, an
    explicit Dispose(), and a drop-on-overwrite (`asg x` reads x) — and an instance already disposed explicitly is live
    to that release and not past it. The first version counted the scope end and Dispose() only; the 5-statement sweep
    then left 6 races (`new a / link a / fmove b a / send a / asg b`), which `selftest0212` now pins.

  REVISION 2 (after the adversarial critique of the first draft, f72926c68). Three more statements, three rule knobs
  and one machine variant, for the places the first draft let a source set be lost:
    newko y x     Make(x, out var y);          an `out owned` recipient of a holder built over x (critic c02)
    wrap y x      var y := Slot.Full(move x);  an owning choice whose owned payload is the holder x (critic c05)
    wuse y        (case y of Full(a) => a.Show())   a member access through the wrapped holder's stored borrow
    out_owned_mint        revised R2: an `out owned` recipient is a mint and carries the call's arguments
    choice_carries        revised R1/R2: a move-only value that owns a holder carries the holder's set
    fmove_carries_holder  revised R2: a value taken out by a replacing `move` carries the binding that holds the
                          instance it was taken from AND that instance's set (critic c03) — whether or not a `link`
                          is visible in this body
    --hidden-link         MACHINE VARIANT: every R is built with its child already linked back to it (the constructor
                          did `Kid := new Child(this)`), which the body's checker cannot see
  The compare command now shows the note's candidate, 0212 as first drafted, 0212 revised, and the revised oracle.

  CRITIQUE ROUND 2 (the re-critique of revision 2, 4dfa0e331, note 2). Two statements, two knobs and one feature
  (`takeout`; feature set `p0212t` = `p0212r` + takeout) for the OPEN REFINEMENT of the taken-out rule (proposal
  §4.7). None of it is proposed text: it measures what the refinement would buy and where it would be unsound.
    kdup y x      var y := x.Rebuild();        a member of the holder K returns `owned K` built over the holder's OWN
                                               source (the critic's e01: `Build() => new Report(_c)`): y never borrows x
    kself y x     var y := x.View();           a member of the holder K returns an owned holder that borrows the RECEIVER
    takeout_by_reach      the refinement: an owned member result carries the holder's binding only where its type can
                          reach the holder's class through borrow fields. `kdup`'s cannot, so it carries the holder's
                          set alone; `kself`'s can, so it still carries the binding
    reach_declared_only   the refinement read off the DECLARED return type of an OPEN class: `kself`'s declared type
                          shows no such field and its dynamic class adds one (probe e02), so the binding is dropped
                          for `kself` too — the model's witness that the test must see every subclass
  `compare --reach` shows 0212 revised, the refinement, and the declared-only reading.

  `selftest0212` runs the worked shapes of the 0212 additions.

  The static-ownership-tracking study's docstring follows.

bzcore_static — the static-ownership-tracking study's model: a COPY of
docs/research/trust-admissions-by-kind/model/bzcore_trust.py (itself a copy of docs/research/soundness/bzcore-l1/bzcore.py),
extended with three statements and five rule knobs. Nothing in the originals is edited.

  Added statements (feature set `static` = locals + fields + holders + holders2):
    kset x y      x.Reassign(y);            a METHOD of the holder K stores its plain parameter in the borrow field
                                            (the K4.3 field-held admission in a non-constructor member; re-points the holder)
    newp x y      var x := new P(new K(y)); a NESTED holder: P owns a K (an `owned` field) that borrows y
    pkuse x       x.k.Show();               a member access through the nested holder's stored borrow
  Added enumeration: --block (prefix ; do … end ; suffix) so a holder can outlive an inner-scope source.

  Added rule knobs:
    bbc               candidate A (borrow-bearing classes): K and P are borrow-bearing by type; a holder VALUE carries the
                      sources of what was stored in it (newk, newp, kset); the carry travels with `move`; K5's freeze and
                      0205's holder rule read it; `kset` is checked as an assignment to the holder's binding (K2.3: the
                      holder may not outlive the source's scope).  The announced kills stay unfrozen (K4.2).
    bbc_static        A with the fully static bound of §28.14 (K5.7): an explicit Dispose() of a carried source is
                      frozen too while the holder is live.
    strict_trust      candidate E: every K4.3 admission (newk, kset, newp, link) is an error.
    scope_end_freeze  the lexical-scope form of the borrow rule under bbc: a carried source may not die (scope end)
                      while the holder is still declared — already implied by K2.3's scope comparison; kept as a check.
    (candidate D is the existing knob K5_unfreeze_dispose=False; candidate B is the existing derived_retention.)

  --v0355 applies the rule flips that bring the v0.345 pin to v0.355: §28.2 (proposal 0205: a dominated use after an
  explicit disposal, through the variable or through a local borrow, is an error) and K5 clause 2's v0.346 sentence
  (#7364: a maybe-consumed LHS is frozen when a still-owning path reaches the store).
  --kdtor is a model variant, not a rule: the holder class K has a destructor that reads its borrowed field, so a
  holder's release (scope end, explicit Dispose) is a use of the stored borrow.

  The trust-shape study's docstring follows.

bzcore_trust — bzcore L1 extended with HOLDER statements for the trust-shape study (a COPY; the original is
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
    bbc=False,                      # candidate A: K and P are borrow-bearing classes; a holder value carries its sources (newk, newp, kset)
    bbc_static=False,               # A, fully static: an explicit Dispose() of a carried source is frozen while the holder is live (K5.7's posture)
    strict_trust=False,             # candidate E: every K4.3 admission is an error
    K5_2_maybe_lhs=False,           # v0.346 (#7364): a maybe-consumed LHS is frozen when a still-owning path reaches the store (K5 clause 2)
    k_dtor_reads=False,             # MODEL VARIANT (not a rule): K has a destructor that reads its borrowed field, so a holder's release is a use
    erasure_rule=False,             # 0212 R5: an owned conversion of a value that carries sources to a type that announces none is rejected
    mint_stored_only=False,         # oracle (A-explicit): a mint carries only the arguments the constructor stores
    q_dtor_reads=False,             # MODEL VARIANT (not a rule): Q has a destructor that reads its back-pointer (the destructor window)
    teardown_live=True,             # MODEL VARIANT: §28.12 (v0.355) — the flag faults from the COMPLETION of Dispose(); False = at its beginning (r40)
    out_owned_mint=False,           # 0212 rev 2, R2: an `out owned` recipient is a mint (carries the call's arguments)
    choice_carries=False,           # 0212 rev 2: a move-only value that owns a holder carries the holder's set
    fmove_carries_holder=False,     # 0212 rev 2, R2: a replacing move's result carries the holder's binding and its set
    hidden_link=False,              # MODEL VARIANT: every R's child is linked back to it at construction (invisible to the checker)
    takeout_by_reach=False,         # OPEN REFINEMENT (0212 §4.7, not proposed text): a member's owned result carries the holder's binding only where its type can reach the holder's class
    reach_declared_only=False,      # the refinement read off the declared type of an open class: the binding is dropped for `kself` as well (unsound; probe e02)
)
# Overridable predicates for user-defined mutations (see `check --define`). Each receives the
# checker's inputs and returns the clause's answer; None means "use the kernel's text".
HOOKS = dict(
    frozen=None,     # frozen(ids, G, live) -> bool            K5's criterion (default: a live borrow sources one of ids)
    meet=None,       # meet(a, b) -> state | raise Reject      K7's cell for two states (default: the K7 table)
)

TYPES_R = 'R'; TYPES_Q = 'Q'; TYPES_K = 'K'; TYPES_P = 'P'; TYPES_E = 'E'; TYPES_W = 'W'    # E: a K held at a type that announces no borrow; W: an owning choice value that owns a K
HOLDER_TYPES = (TYPES_K, TYPES_P)
HOLDER_READS = ('kuse', 'puse', 'pkuse', 'kdtor', 'euse', 'qdtor', 'wuse')

# ----------------------------------------------------------------------------- syntax
# A program is a list of statements; a statement is a tuple:
#   ('new',x) ('decl',x) ('using',x) ('brw',y,x) ('move',y,x) ('usingm',y,x) ('usingb',y,x)
#   ('asg',x) ('asgb',x,y) ('asgm',x,y) ('disp',x) ('send',x) ('use',x)
#   ('fst',x) ('fstm',x,y) ('fbrw',y,x) ('fmove',y,x)
#   ('tuse',) ('tdisp',) ('tsend',) ('tbrw',y) ('throw',x) ('tthrow',)
#   ('if',[A],[B]) ('block',[S]) ('try',[S],e,[H]) ('while',[S])
# A statement's SITE is its position path in the program (a tuple), which also serves as
# the allocation-site identity of the value a producer at that site mints.

DECL_KINDS = ('new', 'decl', 'using', 'brw', 'move', 'usingm', 'usingb', 'fbrw', 'fmove', 'tbrw', 'newk', 'newp', 'newk2', 'erase', 'newko', 'wrap', 'kdup', 'kself')
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
    if k in ('brw', 'move', 'usingm', 'usingb', 'fbrw', 'fmove', 'asgb', 'asgm', 'erase', 'wrap', 'newko', 'kdup', 'kself'):
        return {s[2]}
    if k in ('use', 'disp', 'send', 'throw', 'fst', 'kuse', 'link', 'puse', 'pkuse', 'euse', 'wuse'):
        return {s[1]}
    if k == 'newk2':
        return {s[2], s[3]}
    if k == 'asg' and RULES['q_dtor_reads'] and RULES['bbc']:
        return {s[1]}       # 0212 R3: a drop-on-overwrite is a release point, and a release that may run user code is a use
    if k in ('newk', 'newp'):
        return {s[2]}
    if k == 'kset':
        return {s[1], s[2]}
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
        self.dorder = {}         # name -> declaration order (the declared-before test of §28.14, read for a holder with a destructor)

def ids_of(st):
    return st[1] if st[0] in ('own', 'brw', 'disposed', 'maybe') else frozenset()

def carried(y, G):
    """the sources a HOLDER binding carries under derived retention: its own K-carry, or, for a borrow of a
    holder, the carry of the holder bindings it is bounded by (transitively one hop: the fragment's depth)."""
    if not (RULES['derived_retention'] or RULES['linked_fmove_carries'] or RULES['bbc']):
        return frozenset()
    c = G.get('__carry', {})
    out = frozenset(c.get(y, frozenset()))
    st = G.get(y)
    if st and st[0] == 'brw':
        for b in st[3]:
            out |= c.get(b, frozenset())
    return out

def carried_bindings(y, G):
    """the BINDINGS whose scope bounds what y carries (the `__carryb` side of `carried`)."""
    cb = G.get('__carryb', {})
    out = frozenset(cb.get(y, frozenset()))
    st = G.get(y)
    if st and st[0] == 'brw':
        for b in st[3]:
            out |= cb.get(b, frozenset())
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
        if carried(y, G) & ids and not (st and st[0] == 'disposed'):
            return True     # (0212: an explicitly disposed instance has been released; it is live to that release, not past it)
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
    if RULES['bbc'] and st[0] in ('own', 'brw', 'disposed'):
        # §28.2 (0205's holder rule), read for a class holder under candidate A: ANY use of a holder — a member access,
        # a re-borrow, passing it — after an explicit disposal of every value in its source set on every path is an error.
        cs = carried(x, G)
        if cs and cs <= G['__killed']:
            raise Reject('0205-holder-use-after-dominating-kill')

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
    G = {'__killed': frozenset(), '__carry': {}, '__carryb': {}, '__linked': frozenset()}
    try:
        check_body(prog, (), G, frozenset(), frozenset(), ctx)
    except Reject as r:
        return f'reject:{r.args[0]}', sorted({w[1] for w in ctx.warns}), ctx
    return 'accept', sorted({w[1] for w in ctx.warns}), ctx

def check_body(prog, path, G, live_out, exc_live, ctx, pre_declared=(), depth=0):
    """The statement judgment over one body; returns Γ at the body's end (None if unreachable)."""
    held = set()
    if RULES['k_dtor_reads'] and (RULES['bbc'] or RULES['derived_retention']):
        ty = dict(ctx.types)
        for s0 in prog:                                 # the owned holders this body declares: their scope-end release runs the destructor
            if s0[0] in ('newk', 'newp', 'newk2', 'newko', 'kdup', 'kself'): ty[s0[1]] = TYPES_K; held.add(s0[1])
            elif s0[0] == 'wrap': ty[s0[1]] = TYPES_W; held.add(s0[1])      # the choice's release disposes the K it owns
            elif s0[0] in ('move', 'usingm') and ty.get(s0[2]) in HOLDER_TYPES + (TYPES_W,): ty[s0[1]] = ty[s0[2]]; held.add(s0[1])
            elif s0[0] in DECL_KINDS: ty[s0[1]] = TYPES_R
    if RULES['q_dtor_reads'] and RULES['bbc']:
        # 0212 R3: where a release may run user code that reads a stored borrow, the release is a use. With --qdtor that
        # code is a child Q's destructor, so the owned R and Q bindings this body declares are live to their scope end
        # (it matters only for one that carries a source); K and P hold no Q and stay live to their last use.
        ty2 = dict(ctx.types)
        for s0 in prog:
            if s0[0] in ('new', 'using'): ty2[s0[1]] = TYPES_R; held.add(s0[1])
            elif s0[0] == 'fmove': ty2[s0[1]] = TYPES_Q; held.add(s0[1])
            elif s0[0] in ('move', 'usingm') and ty2.get(s0[2]) in (TYPES_R, TYPES_Q): ty2[s0[1]] = ty2[s0[2]]; held.add(s0[1])
            elif s0[0] in ('newk', 'newk2', 'newko', 'kdup', 'kself'): ty2[s0[1]] = TYPES_K
            elif s0[0] == 'wrap': ty2[s0[1]] = TYPES_W
            elif s0[0] == 'newp': ty2[s0[1]] = TYPES_P
            elif s0[0] == 'erase': ty2[s0[1]] = TYPES_E
            elif s0[0] in ('brw', 'usingb') : ty2[s0[1]] = ty2.get(s0[2])
            elif s0[0] in ('fbrw', 'tbrw'): ty2[s0[1]] = TYPES_Q
            elif s0[0] == 'decl': ty2[s0[1]] = TYPES_R
    la = annotate(prog, frozenset(live_out) | held, exc_live)[1]
    declared = list(pre_declared)
    for v in pre_declared: ctx.depth[v] = depth
    loop = ctx.loop_stack[0] if ctx.loop_stack else None
    for i, s in enumerate(prog):
        site = path + (i,)
        live = la[i]
        ctx.order += 1; order = ctx.order
        k = s[0]
        if k in DECL_KINDS: ctx.dorder[s[1]] = order
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
            if x in G['__carryb']:
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][y] = G['__carryb'][x]
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
            if (st[0] == 'own' or (st[0] == 'maybe' and RULES['K5_2_maybe_lhs'])) and RULES['K5_freeze_overwrite'] and frozen(st[1], G, live):
                raise Reject('K5.2-frozen-overwrite')       # K5.2: fails (i), the reassignment is not keyword-visible
            if RULES['q_dtor_reads'] and RULES['bbc'] and st[0] == 'own' and ctx.types.get(x) in (TYPES_R, TYPES_Q):
                cs = carried(x, G)
                if cs and cs <= G['__killed']:              # the overwrite releases the displaced value: 0205's holder rule at a release that is a use
                    raise Reject('0205-holder-release-after-dominating-kill')
            ctx.asg_static[site] = st[0]; ctx.releases.append((order, x, st[0], loop))
            G[x] = ('own', frozenset({site}))               # moved/disposed/maybe LHS: owned-state, displaces nothing or per path
            if RULES['bbc'] and (x in G['__carry'] or x in G['__carryb']):
                c = dict(G['__carry']); cb = dict(G['__carryb']); c.pop(x, None); cb.pop(x, None)   # the new value is fresh: it carries nothing
                G['__carry'] = c; G['__carryb'] = cb
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
            if (sx[0] == 'own' or (sx[0] == 'maybe' and RULES['K5_2_maybe_lhs'])) and RULES['K5_freeze_overwrite'] and frozen(sx[1], G, live):
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
                if ((RULES['k_dtor_reads'] and (RULES['bbc'] or RULES['derived_retention'])) or (RULES['q_dtor_reads'] and RULES['bbc'] and ctx.types.get(x) in (TYPES_R, TYPES_Q))) and st[0] == 'own':
                    cs = carried(x, G)
                    if cs and cs <= G['__killed']:
                        raise Reject('0205-holder-release-after-dominating-kill')
                if RULES['bbc'] and RULES['bbc_static'] and any(carried(y2, G) & ids for y2 in live):
                    raise Reject('A-static-frozen-dispose-under-holder')    # K5.7's posture: the bound is static, so the flag is no backstop
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
            cy = carried(y, G); cyb = frozenset(G['__carryb'].get(y, frozenset()))
            consume_check(y, G, site, ctx, live, True)
            G[y] = ('moved',)
            if RULES['bbc'] and cy:
                # 0212 R2 clause 2: an owned argument that carries sources brings them to the receiver; checked as an
                # assignment of the updated value to the location that holds the receiver (K2.3, and the declared-before
                # test where the receiver's release runs code that reads the stored borrow).
                hb = frozenset({x}) if G[x][0] != 'brw' else G[x][3]
                if any(ctx.depth.get(sb, 0) > ctx.depth.get(h, 0) for sb in cyb for h in hb):
                    raise Reject('A-K2.3-holder-outlives-source')
                if RULES['q_dtor_reads'] and any(ctx.depth.get(sb, 0) == ctx.depth.get(h, 0) and ctx.dorder.get(sb, 0) > ctx.dorder.get(h, 0) for sb in cyb for h in hb):
                    raise Reject('A-declared-before')
                c = dict(G['__carry']); cb = dict(G['__carryb'])
                for h in hb:
                    c[h] = c.get(h, frozenset()) | cy
                    cb[h] = cb.get(h, frozenset()) | cyb
                G['__carry'] = c; G['__carryb'] = cb
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
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][y] = (frozenset({x}) if G[x][0] != 'brw' else G[x][3])
            if RULES['bbc'] and RULES['fmove_carries_holder']:
                # 0212 rev 2, R2: a value taken out of an instance by a replacing move carries the binding that holds the
                # instance and the instance's set — the body cannot see whether a member linked the child to its owner.
                hx = ids_of(G[x]) if G[x][0] != 'brw' else G[x][1]
                hb = frozenset({x}) if G[x][0] != 'brw' else G[x][3]
                c = dict(G['__carry']); cb = dict(G['__carryb'])
                c[y] = c.get(y, frozenset()) | frozenset(hx) | carried(x, G)
                cb[y] = cb.get(y, frozenset()) | hb | frozenset(G['__carryb'].get(x, frozenset()))
                G['__carry'] = c; G['__carryb'] = cb
            G[y] = ('own', frozenset({site})); ctx.types[y] = TYPES_Q; declared.append(y); ctx.depth[y] = depth
        elif k == 'newk':                               # K4.3 field-held admission through a callee: `var x := new K(y)` where K stores y in a plain field
            x, y = s[1], s[2]
            read_check(y, G, site, ctx)
            srcs = ids_of(G[y]) if G[y][0] != 'brw' else G[y][1]
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
            srcb = frozenset({y}) if G[y][0] != 'brw' else G[y][3]
            G[x] = ('own', frozenset({site})); ctx.types[x] = TYPES_K; declared.append(x); ctx.depth[x] = depth
            if RULES['derived_retention'] or RULES['bbc']:
                G['__carry'] = dict(G['__carry']); G['__carry'][x] = frozenset(srcs)
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][x] = frozenset(srcb)
        elif k == 'newko':                              # `Make(y, out var x)`: an `out owned` recipient of a holder the callee builds over y
            x, y = s[1], s[2]
            read_check(y, G, site, ctx)
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
            srcs = ids_of(G[y]) if G[y][0] != 'brw' else G[y][1]
            srcb = frozenset({y}) if G[y][0] != 'brw' else G[y][3]
            G[x] = ('own', frozenset({site})); ctx.types[x] = TYPES_K; declared.append(x); ctx.depth[x] = depth
            if RULES['bbc'] and RULES['out_owned_mint']:    # rev 2: the recipient is a mint, as an `owned` return is
                G['__carry'] = dict(G['__carry']); G['__carry'][x] = frozenset(srcs)
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][x] = frozenset(srcb)
        elif k in ('kdup', 'kself'):                    # an `owned` result of a member of the holder x: `x.Rebuild()` / `x.View()`
            y, x = s[1], s[2]
            read_check(x, G, site, ctx)
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
            cs = carried(x, G); csb = carried_bindings(x, G)    # the receiver's set
            # R2 clause 1: a mint carries the receiver's contribution — the receiver's binding and its set. The open
            # refinement drops the binding where the result's type cannot reach the holder's class (`kdup`); the
            # declared-only reading drops it for `kself` too, whose dynamic class does hold a borrow of the receiver.
            if not (RULES['takeout_by_reach'] and (k == 'kdup' or RULES['reach_declared_only'])):
                cs |= ids_of(G[x]) if G[x][0] != 'brw' else G[x][1]
                csb |= frozenset({x}) if G[x][0] != 'brw' else G[x][3]
            G[y] = ('own', frozenset({site})); ctx.types[y] = TYPES_K; declared.append(y); ctx.depth[y] = depth
            if RULES['derived_retention'] or RULES['bbc']:
                G['__carry'] = dict(G['__carry']); G['__carry'][y] = frozenset(cs)
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][y] = frozenset(csb)
        elif k == 'wrap':                               # `var y := Slot.Full(move x)`: an owning choice takes the holder as its owned payload
            y, x = s[1], s[2]
            cx = carried(x, G); cxb = frozenset(G['__carryb'].get(x, frozenset()))
            ids = consume_check(x, G, site, ctx, live, True)
            G[x] = ('moved',); G[y] = ('own', ids); ctx.types[y] = TYPES_W; declared.append(y); ctx.depth[y] = depth
            if RULES['bbc'] and RULES['choice_carries'] and cx:   # rev 2: the case constructor is a mint; the value carries what it owns
                c = dict(G['__carry']); cb = dict(G['__carryb']); c[y] = cx; cb[y] = cxb
                G['__carry'] = c; G['__carryb'] = cb
        elif k == 'wuse':                               # a member access through the wrapped holder's stored borrow
            x = s[1]
            read_check(x, G, site, ctx)
            cs = carried(x, G)
            if cs and cs <= G['__killed']:
                raise Reject('0205-holder-use-after-dominating-kill')
        elif k == 'newk2':                              # `var x := new K2(y, z)`: the constructor stores y and only reads z; the mint is read from the signature
            x, y, z = s[1], s[2], s[3]
            read_check(y, G, site, ctx); read_check(z, G, site, ctx)
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
            srcs = frozenset(); srcb = frozenset()
            for a_ in ((y,) if RULES['mint_stored_only'] else (y, z)):   # 0212 R2 clause 1: every plain argument contributes
                srcs |= ids_of(G[a_]) if G[a_][0] != 'brw' else G[a_][1]
                srcb |= frozenset({a_}) if G[a_][0] != 'brw' else G[a_][3]
            G[x] = ('own', frozenset({site})); ctx.types[x] = TYPES_K; declared.append(x); ctx.depth[x] = depth
            if RULES['derived_retention'] or RULES['bbc']:
                G['__carry'] = dict(G['__carry']); G['__carry'][x] = frozenset(srcs)
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][x] = frozenset(srcb)
        elif k == 'erase':                              # `var y: Object := move x`: an owned conversion to a type that announces no borrow
            y, x = s[1], s[2]
            cx = carried(x, G)
            ids = consume_check(x, G, site, ctx, live, True)
            if RULES['bbc'] and RULES['erasure_rule'] and cx:
                raise Reject('0212-owned-erasure')          # R5: the declared type announces nothing, so no later rule could bound the value
            G[x] = ('moved',); G[y] = ('own', ids); ctx.types[y] = TYPES_E; declared.append(y); ctx.depth[y] = depth
            # no carry travels: that is what the position's declared type means
        elif k == 'euse':                               # a member access through the erased holder (its override reads the stored borrow)
            read_check(s[1], G, site, ctx)
        elif k == 'newp':                               # a nested holder: `var x := new P(new K(y))` — P owns a K that stores y (K4.3 at K's constructor)
            x, y = s[1], s[2]
            read_check(y, G, site, ctx)
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
            srcs = ids_of(G[y]) if G[y][0] != 'brw' else G[y][1]
            srcb = frozenset({y}) if G[y][0] != 'brw' else G[y][3]
            G[x] = ('own', frozenset({site})); ctx.types[x] = TYPES_P; declared.append(x); ctx.depth[x] = depth
            if RULES['bbc']:                            # A: P is borrow-bearing (an owned field of borrow-bearing type); 0132's per-parameter bit does not see it
                G['__carry'] = dict(G['__carry']); G['__carry'][x] = frozenset(srcs)
                G['__carryb'] = dict(G['__carryb']); G['__carryb'][x] = frozenset(srcb)
        elif k == 'kset':                               # K4.3 field-held admission in a METHOD: `x.Reassign(y)` re-points the holder's borrow
            x, y = s[1], s[2]
            read_check(x, G, site, ctx); read_check(y, G, site, ctx)
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
            srcs = ids_of(G[y]) if G[y][0] != 'brw' else G[y][1]
            srcb = frozenset({y}) if G[y][0] != 'brw' else G[y][3]
            hb = frozenset({x}) if G[x][0] != 'brw' else G[x][3]      # the binding(s) that hold the holder object
            if RULES['bbc']:
                # checked as an assignment of the updated value to the location that holds it (§9.7 *Member stores*, read for a class holder):
                # K2.3 — the holder's binding may not have a wider scope than a source's.
                if any(ctx.depth.get(sb, 0) > ctx.depth.get(h, 0) for sb in srcb for h in hb):
                    raise Reject('A-K2.3-holder-outlives-source')
                if RULES['k_dtor_reads'] and any(ctx.depth.get(sb, 0) == ctx.depth.get(h, 0) and ctx.dorder.get(sb, 0) > ctx.dorder.get(h, 0) for sb in srcb for h in hb):
                    raise Reject('A-declared-before')        # §28.14's test: the source must be declared before the holder, which is released first
                c = dict(G['__carry']); cb = dict(G['__carryb'])
                for h in hb:
                    c[h] = c.get(h, frozenset()) | frozenset(srcs)          # a member store adds, never removes
                    cb[h] = cb.get(h, frozenset()) | frozenset(srcb)
                G['__carry'] = c; G['__carryb'] = cb
        elif k == 'pkuse':                              # a member access through the nested holder's stored borrow
            x = s[1]
            read_check(x, G, site, ctx)
            cs = carried(x, G)
            if cs and cs <= G['__killed']:
                raise Reject('0205-holder-use-after-dominating-kill')
        elif k == 'kuse':                               # a member access through the holder's stored borrow
            x = s[1]
            read_check(x, G, site, ctx)
            cs = carried(x, G)
            if cs and cs <= G['__killed']:              # 0205's holder rule under derived retention: the kill dominates the use
                raise Reject('0205-holder-use-after-dominating-kill')
        elif k == 'link':                               # the owned child's back-pointer to its owner (`x.f.Attach(x)`): admitted (parameter-sourced in the callee)
            x = s[1]
            read_check(x, G, site, ctx)
            if RULES['strict_trust']:
                raise Reject('E-strict-trust-admission')
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
                    Gr = {v: Gt[v] for v in G if not v.startswith('__')}; Gr['__killed'] = Gt['__killed']; Gr['__carry'] = Gt['__carry']; Gr['__carryb'] = Gt['__carryb']; Gr['__linked'] = Gt['__linked']   # bindings the body declared are out of scope (unwound)
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
        if RULES['k_dtor_reads'] and (RULES['bbc'] or RULES['derived_retention']):
            for v in declared:
                if G[v][0] == 'own' and ctx.types.get(v) in HOLDER_TYPES + (TYPES_W,):
                    cs = carried(v, G)
                    if cs and cs <= G['__killed']:
                        raise Reject('0205-holder-release-after-dominating-kill')
        if RULES['q_dtor_reads'] and RULES['bbc']:
            for v in declared:
                if G[v][0] == 'own' and ctx.types.get(v) in (TYPES_R, TYPES_Q):
                    cs = carried(v, G)
                    if cs and cs <= G['__killed']:
                        raise Reject('0205-holder-release-after-dominating-kill')
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
        elif v in ('__carry', '__carryb'):
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
        self.dangling = False  # some live holder's stored borrow targets a disposed or foreign object at some point of the run
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
        if RULES['hidden_link'] and slot is not None:
            self.heap[slot][3] = self.n                             # the constructor linked the child back to its owner
        return self.n

    def dispose(self, loc, where):
        """DISPOSE(l): first call sets the flag, emits disp(l) and cascades into the owned slot
        (K9); a later call is the idempotent no-op — a read of the flag (K4.1)."""
        st = self.heap[loc]
        if st[0] == 'live':
            if not RULES['teardown_live']:
                st[0] = 'disposed'; self.mark_dangling(loc)          # r40: the flag is set when Dispose() begins
            else:
                st[0] = 'disposing'                                  # §28.12 (v0.355): live to the disposal's own accesses
            self.events.append(('disp', loc))
            if RULES['k_dtor_reads'] and st[2] == TYPES_K:
                self.through(st[1], ('kdtor', where))    # the destructor body reads the borrowed field (K4.1's flag test on the target)
            if RULES['q_dtor_reads'] and st[2] == TYPES_Q and st[3] is not None:
                self.through(st[3], ('qdtor', where))    # the child's destructor body reads its back-pointer
            if st[1] is not None and st[2] in (TYPES_R, TYPES_P):   # K's slot is a BORROW: no cascade; P owns its K
                self.dispose(st[1], 'cascade')
            st[0] = 'disposed'
            self.mark_dangling(loc)
        elif st[0] == 'disposing':
            self.events.append(('noop', loc, where))                 # a re-entrant call during the disposal: the idempotent no-op
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
        if st[1] is not None and st[2] in (TYPES_R, TYPES_P):
            self.free(st[1])

    def mark_dangling(self, loc):
        for l2, s2 in self.heap.items():
            if s2[2] == TYPES_K and s2[0] == 'live' and s2[1] == loc:
                self.dangling = True
            if RULES['q_dtor_reads'] and s2[2] == TYPES_Q and s2[0] == 'live' and s2[3] == loc:
                self.dangling = True

    def foreign(self, loc):
        st = self.heap[loc]
        st[0] = 'foreign'
        self.mark_dangling(loc)
        if st[1] is not None and st[2] in (TYPES_R, TYPES_P):
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
        elif k == 'newko':
            yl = self.binding(s[2])
            env[s[1]] = (self.alloc(TYPES_K, borrow=yl), 'own'); declared.append(s[1])
        elif k == 'kdup':                               # the member reads the receiver's field and builds a holder over the SAME source
            loc = self.read(s[2])
            env[s[1]] = (self.alloc(TYPES_K, borrow=heap[loc][1]), 'own'); declared.append(s[1])
        elif k == 'kself':                              # the member builds a holder over the receiver itself
            loc = self.read(s[2])
            env[s[1]] = (self.alloc(TYPES_K, borrow=loc), 'own'); declared.append(s[1])
        elif k == 'wrap':
            loc = self.binding(s[2])
            env[s[2]] = (loc, 'moved'); env[s[1]] = (loc, 'own'); declared.append(s[1])
        elif k == 'wuse':
            loc = self.read(s[1]); self.through(heap[loc][1], ('wuse', s[1]))
        elif k == 'newk2':
            yl = self.binding(s[2]); self.binding(s[3])              # the constructor reads z and stores y
            env[s[1]] = (self.alloc(TYPES_K, borrow=yl), 'own'); declared.append(s[1])
        elif k == 'erase':
            loc = self.binding(s[2])
            env[s[2]] = (loc, 'moved'); env[s[1]] = (loc, 'own'); declared.append(s[1])
        elif k == 'euse':
            loc = self.read(s[1]); self.through(heap[loc][1], ('euse', s[1]))
        elif k == 'kuse':
            loc = self.read(s[1]); self.through(heap[loc][1], ('kuse', s[1]))
        elif k == 'kset':
            loc = self.read(s[1]); yl = self.binding(s[2]); heap[loc][1] = yl
        elif k == 'newp':
            yl = self.binding(s[2])
            kl = self.alloc(TYPES_K, borrow=yl)
            self.n += 1; heap[self.n] = ['live', kl, TYPES_P, None]; self.events.append(('new', self.n))
            env[s[1]] = (self.n, 'own'); declared.append(s[1])
        elif k == 'pkuse':
            loc = self.read(s[1]); kl = heap[loc][1]
            if heap[kl][0] != 'live': raise Halt('FAULT', ('pkuse', s[1]))
            self.through(heap[kl][1], ('pkuse', s[1]))
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
    'static': {'locals', 'fields', 'holders', 'holders2'},
    'holders-only': {'locals', 'holders', 'holders2'},
    'p0212': {'locals', 'fields', 'holders', 'holders2', 'mint2', 'erase'},
    'p0212-erase': {'locals', 'fields', 'holders', 'holders2', 'erase'},
    'p0212-mint': {'locals', 'fields', 'holders', 'holders2', 'mint2'},
    'p0212r': {'locals', 'fields', 'holders', 'holders2', 'mint2', 'erase', 'out', 'choice'},
    'p0212t': {'locals', 'fields', 'holders', 'holders2', 'mint2', 'erase', 'out', 'choice', 'takeout'},
}

def decl_type(decl, s):
    k = s[0]
    if k in ('new', 'decl', 'using'): return TYPES_R
    if k in ('brw', 'move', 'usingm', 'usingb'): return decl[s[2]]
    if k in ('fbrw', 'fmove', 'tbrw'): return TYPES_Q
    if k in ('newk', 'newk2', 'newko', 'kdup', 'kself'): return TYPES_K
    if k == 'wrap': return TYPES_W
    if k == 'newp': return TYPES_P
    if k == 'erase': return TYPES_E
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
                    if 'holders' in feats and decl[y] not in HOLDER_TYPES and decl[y] not in (TYPES_E, TYPES_W): yield ('newk', x, y)
                    if 'holders2' in feats and decl[y] == TYPES_R: yield ('newp', x, y)
                    if 'erase' in feats and decl[y] == TYPES_K: yield ('erase', x, y)
                    if 'choice' in feats and decl[y] == TYPES_K: yield ('wrap', x, y)
                    if 'out' in feats and decl[y] not in HOLDER_TYPES and decl[y] not in (TYPES_E, TYPES_W): yield ('newko', x, y)
                    if 'takeout' in feats and decl[y] == TYPES_K: yield ('kdup', x, y); yield ('kself', x, y)
                    if 'mint2' in feats and decl[y] not in HOLDER_TYPES and decl[y] not in (TYPES_E, TYPES_W):
                        for z in vs:
                            if z != y and z in decl and decl[z] not in HOLDER_TYPES and decl[z] not in (TYPES_E, TYPES_W): yield ('newk2', x, y, z)
        else:
            if 'locals' in feats and decl[x] == TYPES_E:
                yield ('disp', x); yield ('euse', x)       # an erased holder: its static type offers no reassignment and no send
            elif 'locals' in feats and decl[x] == TYPES_W:
                yield ('disp', x); yield ('wuse', x)       # an owning choice value: a discard (written disp), and a match that reads the payload
            elif 'locals' in feats and decl[x] in HOLDER_TYPES:
                yield ('disp', x); yield ('use', x)        # a holder: no reassignment form in the grammar; not sendable (it holds a borrow)
            elif 'locals' in feats:
                yield ('asg', x); yield ('disp', x); yield ('send', x); yield ('use', x)
            if 'holders' in feats and decl[x] == TYPES_K: yield ('kuse', x)
            if 'holders2' in feats and decl[x] == TYPES_P: yield ('pkuse', x)
            if 'holders' in feats and decl[x] == TYPES_R: yield ('link', x)
            if 'holders' in feats and decl[x] == TYPES_Q: yield ('puse', x)
            if 'fields' in feats and decl[x] == TYPES_R: yield ('fst', x)
            if 'throw' in feats and last and decl[x] == TYPES_R: yield ('throw', x)
            for y in vs:
                if y != x and y in decl:
                    if 'locals' in feats and decl[x] == decl[y] and decl[x] not in HOLDER_TYPES and decl[x] not in (TYPES_E, TYPES_W): yield ('asgb', x, y)
                    if 'asgm' in feats and decl[x] == decl[y] and decl[x] not in HOLDER_TYPES and decl[x] not in (TYPES_E, TYPES_W): yield ('asgm', x, y)
                    if 'holders2' in feats and decl[x] == TYPES_K and decl[y] == TYPES_R: yield ('kset', x, y)
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

WITH_BLOCK = [False]

def enum_programs(vs, maxlen, feats, with_if=False, with_try=False, with_while=False, body=2):
    """Deterministic, bounded: straight-line bodies of length 1..maxlen; with a construct:
    prefix (1..maxlen-2) ; construct ; suffix (0..1).  Arms and handlers hold ≤1 statement and
    declare nothing; try/while bodies hold 1..`body` statements and may declare."""
    for n in range(1, maxlen + 1):
        yield from enum_straight(vs, n, {}, feats)
    with_block = WITH_BLOCK[0]
    if not (with_if or with_try or with_while or with_block):
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
        if with_block:
            bodies = list(enum_upto(vs, body, d, feats, nmin=1, allow_throw=False))
            for b in bodies:
                for suf in suffixes:
                    yield pre + [('block', b)] + suf

# ----------------------------------------------------------------------------- sweeps

class Row:
    """An accepted program's record: warnings, oracle verdicts, a hash of its disposal traces
    (order included), the per-path outcomes and the release-site flag reads.  A rejected program
    is recorded as its verdict string only (memory: sweeps reach 10^6 programs)."""
    __slots__ = ('prog', 'verdict', 'warns', 'bad', 'trace', 'outs', 'noops', 'hold', 'dang')
    def __init__(self, prog, verdict, warns, bad=(), trace=0, outs=(), noops=None):
        self.prog = prog; self.verdict = verdict; self.warns = warns; self.bad = bad
        self.trace = trace; self.outs = outs; self.noops = noops or {}; self.hold = ''; self.dang = False

def evaluate(prog):
    v, w, ctx = check(prog)
    if v != 'accept':
        return Row(prog, v, tuple(w)), ctx
    runs = run_all(prog, ctx)
    bad = sorted(set(judge(runs)))
    if bad == ['RACE'] and any(r.outcome == 'RACE' and isinstance(r.info, tuple) and r.info[0] in HOLDER_READS for r in runs) and not any(r.outcome == 'RACE' and not (isinstance(r.info, tuple) and r.info[0] in HOLDER_READS) for r in runs):
        bad = ['K4.3-trust-residual']
    elif bad == ['RACE'] and ctx.taint and any(s[0] in ('send',) for s in iter_stmts(prog)):
        # K4.3's recorded slot-kill-by-move clause (§28.3.9 Scope, clause 1; §28.8.1): a value the replacing
        # move took out of a slot while a borrow of the holder was live, then transferred to another domain —
        # the spec admits it as a recorded trust obligation, not as a checked rule; T3 holds only under it.
        bad = ['K4.3-trust-residual']
    trace = hash(tuple(tuple(e for e in r.events if e[0] in ('new', 'disp', 'noop', 'release', 'free')) + (r.outcome,) for r in runs))
    noops, outs = trace_facts(runs)
    row = Row(prog, v, tuple(w), tuple(bad), trace, tuple(outs), dict(noops))
    # the trust surface, per program: does some execution read a disposed (FAULT) or foreign (RACE) object THROUGH A STORED BORROW?
    hf = [r.outcome for r in runs if r.outcome in ('FAULT', 'RACE') and isinstance(r.info, tuple) and r.info[0] in HOLDER_READS]
    row.hold = ('race' if 'RACE' in hf else ('every' if len(hf) == len(runs) else 'some')) if hf else ''
    row.dang = any(r.dangling for r in runs)
    return row, ctx

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
            if any(s[0] in ('newk', 'kset', 'newp', 'link', 'newk2', 'newko', 'kdup', 'kself') for s in iter_stmts(r.prog)): st['programs-with-a-trust-store'] += 1
            if r.hold: st['stale-read-through-a-stored-borrow:' + r.hold] += 1
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
    s += ''.join(f' --{f}' for f in ('if', 'try', 'while') if getattr(a, 'with_' + f)) + (' --block' if WITH_BLOCK[0] else '') + (' [v0.355 base]' if getattr(a, 'v0355', False) else '') + (' [child destructor reads its back-pointer]' if RULES['q_dtor_reads'] else '') + (' [every child is linked to its owner at construction]' if RULES['hidden_link'] else '') + ('' if RULES['teardown_live'] else ' [flag set when Dispose() begins: r40]') + (' [holder destructor reads its borrow]' if RULES['k_dtor_reads'] else '')
    if WITH_BLOCK[0] and not (a.with_try or a.with_while): s += f' --body {a.body}'
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
    'A: borrow-bearing classes (holder values carry their sources; announced kills stay unfrozen)': dict(bbc=True, linked_fmove_carries=True),
    'A-static: borrow-bearing classes with the fully static bound (Dispose of a carried source frozen under a live holder)': dict(bbc=True, bbc_static=True, linked_fmove_carries=True),
    'B: derived retention (idealised: the carry follows a moved holder) + sharp owner-outlives': dict(derived_retention=True, linked_fmove_carries=True),
    'D: stricter freeze (explicit Dispose() frozen under any live local borrow)': dict(K5_unfreeze_dispose=False),
    'A+D: borrow-bearing classes and the stricter freeze': dict(bbc=True, linked_fmove_carries=True, K5_unfreeze_dispose=False),
    'E: strict mode (every K4.3 admission is an error)': dict(strict_trust=True),
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

# ----------------------------------------------------------------------------- candidate comparison (this study)

REV2 = dict(bbc=True, linked_fmove_carries=True, erasure_rule=True, out_owned_mint=True, choice_carries=True, fmove_carries_holder=True)
CANDIDATES = [
    ('A  the candidate of the note (no erasure rule)', dict(bbc=True, linked_fmove_carries=True)),
    ('0212-draft1  0212 as first drafted (A + the owned-erasure rule)', dict(bbc=True, linked_fmove_carries=True, erasure_rule=True)),
    ('0212  revised: out-owned mint, move-only carriers, replacing move carries its holder', dict(REV2)),
    ('0212-explicit  revised, with a stored-arguments-only mint (oracle)', dict(REV2, mint_stored_only=True)),
]

# Critique round 2, note 2: the open refinement of the taken-out rule (`compare --reach`, feature set p0212t).
REACH_CANDIDATES = [
    ('0212  revised: a member\'s owned result carries the holder\'s binding and its set', dict(REV2)),
    ('0212-reach  the refinement: the binding is carried only where the result\'s type can reach its holder', dict(REV2, takeout_by_reach=True)),
    ('0212-reach-declared  the refinement read off the declared type of an open class (unsound)', dict(REV2, takeout_by_reach=True, reach_declared_only=True)),
]

def base_class(r):
    """How a baseline-accepted program behaves at run time: the hazard class of a rejection."""
    if r.hold == 'race': return 'race'                    # a read of an object another domain owns, through a stored borrow (no flag: K4.3's domain clause)
    if any(o == 'RACE' for o in r.outs): return 'race'
    fp = fault_profile(r.outs)
    if fp == 'every': return 'every' + ('(holder)' if r.hold else '(local)')
    if fp == 'some': return 'some' + ('(holder)' if r.hold else '(local)')
    return 'never(dangling)' if r.dang else 'never(clean)'

def cmd_compare(a, vs, feats):
    t0 = time.time()
    base, _ = sweep(a, vs, feats)
    bs = summarize(base)
    acc = accepted(base)
    hold = Counter(r.hold for r in acc if r.hold)
    print(f'# Candidate comparison — bounds: {bounds_str(a)}')
    print(f'baseline: {bs["programs"]} programs, {bs["accept"]} accepted; {bs["programs-with-a-trust-store"]} accepted programs contain a K4.3 store; '
          f'VIOLATIONS {bs["VIOLATIONS"]}; K4.3 race residuals {bs["K4.3-trust-residual (T3 under the recorded clause)"]}')
    print(f'baseline trust surface (accepted programs in which some run reads a dead or foreign object THROUGH A STORED BORROW): '
          f'{sum(hold.values())} = race {hold["race"]} + fault on every run {hold["every"]} + fault on some run {hold["some"]}')
    print(f'baseline flag surface (accepted programs that fault on every / some run, any cause): {bs["fault-every"]} / {bs["fault-some"]}   ({time.time() - t0:.1f} s)\n')
    print('| candidate | accepted | newly accepted | newly unsound | newly rejected | …race | …fault every run (holder/local) | …fault some run (holder/local) | …never fault: a dangling stored borrow exists, unread / no dangling borrow at all | left: race through a stored borrow | left: fault through a stored borrow (every/some) | left: accepted programs with an untracked K4.3 store | witness (clean rejection) |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    notes = []
    for name, flips in CANDIDATES:
        new_acc = []; new_rej = []; newly_unsound = []; n_acc = 0
        res = Counter(); untracked = 0; res_w = {}
        with mutated(flips):
            for k, r, _ in sweep_iter(vs, a.len, feats, a.with_if, a.with_try, a.with_while, a.body):
                bb_ = base[k]
                if r.verdict != 'accept':
                    if isinstance(bb_, Row): new_rej.append(bb_)
                    continue
                n_acc += 1
                if not isinstance(bb_, Row):
                    new_acc.append(r)
                    if r.bad: newly_unsound.append(r)
                elif r.bad and not bb_.bad:
                    newly_unsound.append(r)
                if r.hold:
                    res[r.hold] += 1
                    if r.hold not in res_w or len(oneline(r.prog)) < len(res_w[r.hold]): res_w[r.hold] = oneline(r.prog)
                kinds = {s[0] for s in iter_stmts(r.prog)}
                if flips.get('bbc'):
                    pass                                    # every store is tracked by type
                elif flips.get('derived_retention'):
                    if kinds & {'kset', 'newp'}: untracked += 1      # 0132's bit covers a constructor's direct store only
                elif kinds & {'newk', 'kset', 'newp', 'link', 'newk2', 'newko', 'kdup', 'kself'}:
                    untracked += 1
        cls = Counter(base_class(r) for r in new_rej)
        nv = [r for r in new_rej if base_class(r) == 'never(clean)']
        w = oneline(minimal(nv, 1)[0].prog) if nv else ''
        print(f'| {name} | {n_acc} | {len(new_acc)} | {len(newly_unsound)} | {len(new_rej)} | {cls["race"]} | '
              f'{cls["every(holder)"]}/{cls["every(local)"]} | {cls["some(holder)"]}/{cls["some(local)"]} | {cls["never(dangling)"]} / {cls["never(clean)"]} | '
              f'{res["race"]} | {res["every"]}/{res["some"]} | {untracked} | `{w}` |')
        if res_w:
            notes.append(f'- {name.split("  ")[0]}: residual witnesses — ' + '; '.join(f'{kk}: `{vv}`' for kk, vv in sorted(res_w.items())))
        if newly_unsound:
            notes.append(f'- {name.split("  ")[0]}: NEWLY UNSOUND e.g. `{oneline(minimal(newly_unsound, 1)[0].prog)}`')
        sys.stdout.flush()
    print()
    for n_ in notes: print(n_)
    print(f'\n({time.time() - t0:.1f} s)')

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

P0212 = dict(K5_3b_borrow='error', K5_3b_samevar='error', K5_2_maybe_lhs=True, bbc=True, linked_fmove_carries=True, erasure_rule=True,
             out_owned_mint=True, choice_carries=True, fmove_carries_holder=True)
DRAFT1 = dict(K5_3b_borrow='error', K5_3b_samevar='error', K5_2_maybe_lhs=True, bbc=True, linked_fmove_carries=True, erasure_rule=True)
# (title, program, variant flips, verdict and outcomes under 0212 as first drafted, verdict and outcomes under 0212 revised)
SELFTEST_REV2 = [
    ('critic c02 — `out owned`: the recipient outlives the argument its holder was built over',
     [('decl', 'c'), ('block', [('new', 'a'), ('newko', 'b', 'a'), ('asgm', 'c', 'b')]), ('use', 'c')], {},
     None, None, None, None),     # placeholder, replaced below (asgm is outside the enumerated feature set; kept as a hand case)
    ('critic c02 — `out owned`: the argument is sent to another domain while the recipient is still used',
     [('new', 'a'), ('newko', 'b', 'a'), ('send', 'a'), ('kuse', 'b')], {},
     'accept', ['RACE'], 'reject:K5.1-frozen-transfer', None),
    ('critic c02 — `out owned`: the argument is disposed, then the recipient is used',
     [('new', 'a'), ('newko', 'b', 'a'), ('disp', 'a'), ('kuse', 'b')], {},
     'accept', ['FAULT'], 'reject:0205-holder-use-after-dominating-kill', None),
    ('critic c05 — an owning choice wraps a holder: the source is sent away, then the payload is read',
     [('new', 'a'), ('newk', 'b', 'a'), ('wrap', 'c', 'b'), ('send', 'a'), ('wuse', 'c')], {},
     'accept', ['RACE'], 'reject:K5.1-frozen-transfer', None),
    ('critic c05 — an owning choice wraps a holder: the source is disposed, then the payload is read',
     [('new', 'a'), ('newk', 'b', 'a'), ('wrap', 'c', 'b'), ('disp', 'a'), ('wuse', 'c')], {},
     'accept', ['FAULT'], 'reject:0205-holder-use-after-dominating-kill', None),
    ('critic c05 — wrapping is not an erasure: with the source alive the wrapped holder is used normally',
     [('new', 'a'), ('newk', 'b', 'a'), ('wrap', 'c', 'b'), ('wuse', 'c')], {},
     'accept', ['ok'], 'accept', ['ok']),
    ('critic c03 — a child linked in its constructor is taken out by a replacing move; the owner is disposed; the child reads it',
     [('new', 'a'), ('fmove', 'b', 'a'), ('disp', 'a'), ('puse', 'b')], dict(hidden_link=True),
     'accept', ['FAULT'], 'reject:0205-holder-use-after-dominating-kill', None),
    ('critic c03 — the same with the owner sent to another domain: a race no flag catches',
     [('new', 'a'), ('fmove', 'b', 'a'), ('send', 'a'), ('puse', 'b')], dict(hidden_link=True),
     'accept', ['RACE'], 'reject:K5.1-frozen-transfer', None),
    ('critic c03 — the child is used while its owner lives: admitted and clean',
     [('new', 'a'), ('fmove', 'b', 'a'), ('puse', 'b')], dict(hidden_link=True),
     'accept', ['ok'], 'accept', ['ok']),
]
SELFTEST_REV2 = [t for t in SELFTEST_REV2 if t[3] is not None]
BASE55 = dict(K5_3b_borrow='error', K5_3b_samevar='error', K5_2_maybe_lhs=True)
SELFTEST_0212 = [
    # (title, program, variant flips, expected verdict at the v0.355 baseline, expected outcomes there, expected verdict under 0212, expected outcomes there)
    ('R5 erasure: a holder converted to a type that announces nothing, its source disposed, then read',
     [('new', 'a'), ('newk', 'b', 'a'), ('erase', 'c', 'b'), ('disp', 'a'), ('euse', 'c')], {},
     'accept', ['FAULT'], 'reject:0212-owned-erasure', None),
    ('R5 erasure: the same with the source sent to another domain (no flag: a race at the baseline)',
     [('new', 'a'), ('newk', 'b', 'a'), ('erase', 'c', 'b'), ('send', 'a'), ('euse', 'c')], {},
     'accept', ['RACE'], 'reject:0212-owned-erasure', None),
    ('R2 mint: a holder carries the argument its constructor only reads, so a transfer of that argument is frozen',
     [('new', 'a'), ('new', 'c'), ('newk2', 'b', 'a', 'c'), ('send', 'c'), ('kuse', 'b')], {},
     'accept', ['ok'], 'reject:K5.1-frozen-transfer', None),
    ('R2 mint: the stored argument is frozen under either mint rule',
     [('new', 'a'), ('new', 'c'), ('newk2', 'b', 'a', 'c'), ('send', 'a'), ('kuse', 'b')], {},
     'accept', ['RACE'], 'reject:K5.1-frozen-transfer', None),
    ('destructor window, §28.12 (v0.355): a child destructor reads its owner in the cascade of its owner, on a live instance',
     [('new', 'a'), ('link', 'a')], dict(q_dtor_reads=True),
     'accept', ['ok'], 'accept', ['ok']),
    ('destructor window on r40: the flag is set when Dispose() begins, so the same read faults',
     [('new', 'a'), ('link', 'a')], dict(q_dtor_reads=True, teardown_live=False),
     'accept', ['FAULT'], 'accept', ['FAULT']),
    ('destructor window: a linked child moved out carries its owner; the owner disposed first makes the release of the child a certain fault',
     [('new', 'a'), ('link', 'a'), ('fmove', 'c', 'a'), ('disp', 'a')], dict(q_dtor_reads=True),
     'accept', ['FAULT'], 'reject:0205-holder-release-after-dominating-kill', None),
    ('destructor window: a linked child moved out and released before its owner is clean',
     [('new', 'a'), ('link', 'a'), ('fmove', 'c', 'a')], dict(q_dtor_reads=True),
     'accept', ['ok'], 'accept', ['ok']),
    ('R3 release: a drop-on-overwrite of a moved-out child is a release, so its owner may not be sent away before it',
     [('new', 'a'), ('link', 'a'), ('fmove', 'b', 'a'), ('send', 'a'), ('asg', 'b')], dict(q_dtor_reads=True),
     'accept', ['RACE'], 'reject:K5.1-frozen-transfer', None),
    ('R3 release: the same overwrite after the owner was disposed is a certain fault',
     [('new', 'a'), ('link', 'a'), ('fmove', 'b', 'a'), ('disp', 'a'), ('asg', 'b')], dict(q_dtor_reads=True),
     'accept', ['FAULT'], 'reject:0205-holder-release-after-dominating-kill', None),
    ('R2: a reassigned binding takes a fresh value and carries nothing, so the old owner may then be sent away',
     [('new', 'a'), ('link', 'a'), ('fmove', 'b', 'a'), ('asg', 'b'), ('send', 'a')], {},
     'accept', ['ok'], 'accept', ['ok']),
    ('R3 release: an instance already disposed explicitly is not live past that release, so its source may then be sent away',
     [('new', 'a'), ('newk', 'b', 'a'), ('disp', 'b'), ('send', 'a')], dict(k_dtor_reads=True),
     'accept', ['ok'], 'accept', ['ok']),
    ('R2 clause 2: a linked child re-homed into another owner brings its old owner to the new one (declared-before)',
     [('new', 'b'), ('new', 'a'), ('link', 'a'), ('fmove', 'c', 'a'), ('fstm', 'b', 'c')], dict(q_dtor_reads=True),
     'accept', ['FAULT'], 'reject:A-declared-before', None),
    ('R2 clause 2: re-homed into an owner declared after the old one is clean',
     [('new', 'a'), ('link', 'a'), ('fmove', 'c', 'a'), ('new', 'b'), ('fstm', 'b', 'c')], dict(q_dtor_reads=True),
     'accept', ['ok'], 'accept', ['ok']),
    ('R2 clause 2: re-homed, then the old owner disposed: the release of the new owner is a certain fault',
     [('new', 'a'), ('link', 'a'), ('fmove', 'c', 'a'), ('new', 'b'), ('fstm', 'b', 'c'), ('disp', 'a')], dict(q_dtor_reads=True),
     'accept', ['FAULT'], 'reject:0205-holder-release-after-dominating-kill', None),
]

REACH = dict(P0212, takeout_by_reach=True)
REACH_DECLARED = dict(P0212, takeout_by_reach=True, reach_declared_only=True)
# (title, program, variant flips, then verdict and outcomes under: 0212 revised; the refinement; its declared-only reading)
SELFTEST_REACH = [
    ('critic e01 — a builder\'s product borrows the builder\'s argument: the builder is moved away while the product lives',
     [('new', 'a'), ('newk', 'b', 'a'), ('kdup', 'c', 'b'), ('move', 'd', 'b'), ('kuse', 'c')], {},
     'reject:K5.1-frozen-transfer', None, 'accept', ['ok'], 'accept', ['ok']),
    ('critic e01 — the product still carries the builder\'s source: that source is sent away, then the product is read',
     [('new', 'a'), ('newk', 'b', 'a'), ('kdup', 'c', 'b'), ('send', 'a'), ('kuse', 'c')], {},
     'reject:K5.1-frozen-transfer', None, 'reject:K5.1-frozen-transfer', None, 'reject:K5.1-frozen-transfer', None),
    ('probe e02 — the product borrows the builder itself: the builder is moved away and disposed, then the product is read',
     [('new', 'a'), ('newk', 'b', 'a'), ('kself', 'c', 'b'), ('move', 'd', 'b'), ('disp', 'd'), ('kuse', 'c')], {},
     'reject:K5.1-frozen-transfer', None, 'reject:K5.1-frozen-transfer', None, 'accept', ['FAULT']),
    ('probe e02 — the same, with the builder released at its scope end before a product whose destructor reads it',
     [('new', 'a'), ('newk', 'b', 'a'), ('kself', 'c', 'b'), ('move', 'd', 'b')], dict(k_dtor_reads=True),
     'reject:K5.1-frozen-transfer', None, 'reject:K5.1-frozen-transfer', None, 'accept', ['FAULT']),
    ('a product that borrows the builder, used while the builder lives: admitted and clean under every reading',
     [('new', 'a'), ('newk', 'b', 'a'), ('kself', 'c', 'b'), ('kuse', 'c')], {},
     'accept', ['ok'], 'accept', ['ok'], 'accept', ['ok']),
]

def cmd_selftest0212():
    fails = 0
    for title, prog, variant, e_base, o_base, e_new, o_new in SELFTEST_0212:
        line = f'--- {title}\n    ' + oneline(prog)
        for tag, flips, expect, outs in (('v0.355', BASE55, e_base, o_base), ('0212', P0212, e_new, o_new)):
            f2 = dict(flips); f2.update(variant)
            with mutated(f2):
                v, w, ctx = check(prog)
                ok = (v == expect); got = None; bad = []
                if v == 'accept':
                    runs = run_all(prog, ctx); got = [r.outcome for r in runs]; bad = sorted(set(judge(runs)))
                    if outs is not None and got != outs: ok = False
            line += f'\n    {tag}: {v}  runs: {got}  bad: {bad}  ' + ('OK' if ok else f'MISMATCH (expected {expect} {outs})')
            if not ok: fails += 1
        print(line)
    for title, prog, variant, e_d1, o_d1, e_new, o_new in SELFTEST_REV2:
        line = f'--- {title}\n    ' + oneline(prog)
        for tag, flips, expect, outs in (('0212 first draft', DRAFT1, e_d1, o_d1), ('0212 revised', P0212, e_new, o_new)):
            f2 = dict(flips); f2.update(variant)
            with mutated(f2):
                v, w, ctx = check(prog)
                ok = (v == expect); got = None; bad = []
                if v == 'accept':
                    runs = run_all(prog, ctx); got = [r.outcome for r in runs]; bad = sorted(set(judge(runs)))
                    if outs is not None and got != outs: ok = False
            line += f'\n    {tag}: {v}  runs: {got}  bad: {bad}  ' + ('OK' if ok else f'MISMATCH (expected {expect} {outs})')
            if not ok: fails += 1
        print(line)
    for title, prog, variant, e_r2, o_r2, e_rf, o_rf, e_dc, o_dc in SELFTEST_REACH:
        line = f'--- {title}\n    ' + oneline(prog)
        for tag, flips, expect, outs in (('0212 revised', P0212, e_r2, o_r2), ('refinement', REACH, e_rf, o_rf), ('refinement, declared type only', REACH_DECLARED, e_dc, o_dc)):
            f2 = dict(flips); f2.update(variant)
            with mutated(f2):
                v, w, ctx = check(prog)
                ok = (v == expect); got = None; bad = []
                if v == 'accept':
                    runs = run_all(prog, ctx); got = [r.outcome for r in runs]; bad = sorted(set(judge(runs)))
                    if outs is not None and got != outs: ok = False
            line += f'\n    {tag}: {v}  runs: {got}  bad: {bad}  ' + ('OK' if ok else f'MISMATCH (expected {expect} {outs})')
            if not ok: fails += 1
        print(line)
    total = 2 * len(SELFTEST_0212) + 2 * len(SELFTEST_REV2) + 3 * len(SELFTEST_REACH)
    print(f'\nselftest0212: {total - fails} passed, {fails} failed')
    return fails

# ----------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['selftest', 'selftest0212', 'enumerate', 'mutate', 'check', 'retention', 'mutations', 'compare'])
    ap.add_argument('--qdtor', action='store_true', help='model variant: the child class Q has a destructor that reads its back-pointer')
    ap.add_argument('--hidden-link', dest='hidden_link', action='store_true', help='model variant: every R is built with its child linked back to it; the checker cannot see the link')
    ap.add_argument('--r40-teardown', dest='r40_teardown', action='store_true', help='model variant: the disposed flag is set when Dispose() begins (r40), not when it completes (spec §28.12, v0.355)')
    ap.add_argument('--reach', action='store_true', help='compare: show 0212 revised against the open refinement of the taken-out rule and its declared-only reading')
    ap.add_argument('--block', dest='with_block', action='store_true', help='also enumerate prefix ; do … end ; suffix')
    ap.add_argument('--kdtor', action='store_true', help='model variant: the holder class K has a destructor that reads its borrowed field')
    ap.add_argument('--v0355', action='store_true', help="apply proposal 0205's two rule flips (the v0.355 baseline)")
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
    WITH_BLOCK[0] = a.with_block
    if a.v0355:
        RULES.update(K5_3b_borrow='error', K5_3b_samevar='error', K5_2_maybe_lhs=True)
    if a.kdtor:
        RULES.update(k_dtor_reads=True)
    if a.qdtor:
        RULES.update(q_dtor_reads=True)
    if a.r40_teardown:
        RULES.update(teardown_live=False)
    if a.hidden_link:
        RULES.update(hidden_link=True)
    if a.cmd == 'selftest0212':
        sys.exit(1 if cmd_selftest0212() else 0)
    if a.cmd == 'compare':
        if a.reach: CANDIDATES[:] = REACH_CANDIDATES
        cmd_compare(a, vs, feats); return
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
