# 0047 - Mechanized core semantics for ownership and domains

**Status:** under-review — draft
**Tracking issue:** [#2034](https://github.com/s8lap/bezel-lang/issues/2034)
**Target:** Verification artifact only — a new top-level `formal/` tree (paper calculus, Lean 4 model, differential-validation bridge), **never a compiler build dependency** (§5.4). No spec, stdlib, grammar, or compiler changes. Two process touch points, both staged: (a) one checklist line added to [`spec-change-process.md`](spec-change-process.md) §9 at stage S3 (a §28/§30 change updates the model or records its exclusion); (b) an **advisory, optional** CI lane alongside the existing `.github/workflows/ci.yml`, never in `scripts/gate.sh`'s blocking path.
**Written against:** spec v0.197 (stdlib/rationale at the same revision)
**Currency note:** Refreshed 2026-08-11 to v0.246: the marked-transfer spelling is `move x` at every consume position (v0.246 unification — the calculus's `"owned" x` marked-transfer form, its Arg alternative, the T-Move/T-Frz/T-Call rule displays, and the §5.3 examples respelled; declaration positions — `Field`/`Param`/`Ret` `owned` — unchanged). The two flagged v0.243/v0.246 interactions are **resolved** (owner's 2026-08-12 direction on the recorded flags): the §5.3 bridge printer derives every thrown class from `Exception`, and the §3.1 A7 note is re-aimed at A7's two real positions — see "S0 design notes — delegated rulings 2026-08-12" below. Re-verified at v0.306 (2026-09-17 sweep): **no tier-1 respells** — every K-id this document cites (K1.5–K9.5) still resolves, the `move` spellings and declaration-position `owned` markers are current, and the rationale citations (§5.1, §5.23, §5.25, §5.26, §12.1–§12.2, §5.3/§5.11) all still resolve. ⚠ **Two tier-2 flags** recorded in `proposals-currency-2026-09-17.md` §3, both against the kernel/axiom ledger this proposal's §4.2/§4.3/§7 turn on: (1) the register is now **A1–A15 — eleven live axioms and four tombstones** (rationale §5.26, v0.306: A1 retired v0.201, A2 retired **v0.258**, A8 retired v0.213, A13 retired **v0.260**; **A15** added **v0.266**), against this document's "A1–A14"/"fourteen recorded axioms" and §4.3's live-axiom dispositions for A2 and A13; (2) v0.257 (0093) named the kill table's three bits and the three announcement senses, and **v0.281 (0118) merged K3's position list, K4's kill table and K5's placement list into one consume table** (spec §28.3, anchor `sec-consume-table`; the K4.2 two-bit reading note moved to rationale §5.26), which is the surface §3.4's rule displays and §5.2's one-file-per-principle mapping mirror. Written-against pin unchanged.

This proposal expands item **P7** of [`future-directions-2026-07-18.md`](papers/future-directions-2026-07-18.md) to commissioning depth. It is the consumer proposal [0046](0046-ownership-kernel-consolidation.md) §11 pre-announces: the kernel principles **K1–K9** and the axiom register **A1–A14** are adopted from 0046 *verbatim* — this document introduces no parallel numbering, and every model artifact cites K/A ids as frozen at 0046's T1 landing. Where this document sharpens or diverges from the P7 sketch, the divergence is recorded in §11. *Reference convention: an unprefixed §-reference is a language-spec reference; `rationale §`, `stdlib §`, and `0046 §` prefixes are written out; this proposal's own sections are cited as "§N below" or from context.*

## S0 design notes — delegated rulings 2026-08-12

*Recorded under the owner's 2026-08-12 direction on the recorded flags ([2026-08-11 currency paper](papers/proposals-currency-2026-08-11.md) §3, flags 3–4). Both are S0-stage design decisions binding on the `formal/calculus/` hardening pass; neither changes a theorem statement or the calculus's rule set.*

1. **The bridge printer derives every thrown class from `Exception` (flag 3 × v0.243).** The calculus has no inheritance — `throw Rhs` throws any owned class — while since v0.243 (proposal 0080) every surface throw operand must be of an `Exception`-derived class type (spec §31.1). A printed `throw x` at a plain generated class would therefore be a compile error, and every model-accepted throw-program would spuriously diff as a `bezelc` reject — a systematic false mismatch in the §5.3 verdict differential. Resolution, option (i) as recommended: **`bzlprint` emits every class that appears as a `throw` operand anywhere in the program with the derivation `public class R: Exception … end R;`** (chaining the base's parameterless `pure` constructor, spec §31.4; classes never thrown print as today, keeping the printer's vocabulary minimal). This is free and **sound**: v0.243's own clause-by-clause verification records the ownership seam as base-class-blind — K3.8/K4.2's announced-kill status, the moved-from machinery, the exception-ownership rules with both dispose-after-arm exemptions, and the pure-throw carve-out's trivial-dispose ground are all unchanged by the base class — and §31.1 explicitly admits owned fields on exception classes, so a thrown calculus class with owned fields prints legally and its disposal cascade (T1's `disp` events, E-CatchDone's dispose-after-arm included) is compared unchanged. Declined: (ii) a calculus thrown-class kind premise — heavier, models a typing fact irrelevant to T1–T3; (iii) excluding throw-programs from the differential — loses coverage of the K3.8/K4.2 announced-kill path, the worst option. A presentation rule of the bridge, recorded here so S1's printer and corpus carry it from the start.

2. **The §3.1 A7 modeling note re-aimed at A7's two real positions (flag 4 × v0.209/v0.246).** As drafted against v0.197, §3.1 said the absence of a transfer marker "in any arm/operand position" was axiom A7 modeled by omission. That overstates A7's scope twice over: arm positions (if-expression branches, expression-`case`/`when` arm results) have carried the K3.4 marker since **v0.209** (proposal 0056 — A7 narrowed, not retired), and **v0.246** rewrote A7's register row again on purely semantic grounds (the unified `move` *parses* in the rejected positions; the rejection is recorded policy, not a missing spelling), its positions unchanged: exactly the **`??`-operand and operator-operand** halves. The note is re-aimed in place (§3.1; §4.3's disposition row): A7-modeled-by-omission means those two positions are absent from the calculus grammar. The calculus's lack of arm/operand *expression positions in general* is a consequence of A-normalization (§2.1), not of A7 — the marker-bearing arm positions the surface has had since v0.209 are outside the v1 fragment as a fragment-scope fact, not an axiom fact. Wording-level, but it names an axiom, so it is recorded as a ruling rather than respelled silently.

## 0. Summary

Build a mechanized model of Bezel's ownership core — the 0046 kernel — in Lean 4, prove the four soundness theorems the language's design claims (exactly-once disposal; deterministic checked-mode use-after-kill; domain isolation; staged, frame-residence soundness), and wire the model to the implementation through a **validation bridge**: calculus programs pretty-printed to real `.bzl` and run through `bezelc`, with accept/reject/warn verdicts compared differentially. The value is near-term bug-finding, not credentialing — the record already contains the theorem this model would have refuted before it shipped: §30.14's isolation claim was **false** for several revisions until a probe-confirmed data race forced the v0.180 consume-site freeze (rationale §5.1). The model carries that episode forward as a mechanized regression: the freeze-necessity lemma (§4.1, T3ₓ) is the v0.180 counterexample, permanently re-checked. The artifact lives in `formal/`, versions in lockstep with the spec revision and the 0046 axiom register (including scheduled register changes such as proposal [0048](0048-explicit-factory-ownership.md)'s deletion of A1), and is explicitly *not* a compiler build dependency — CLAUDE.md's dependency discipline is addressed head-on in §5.4 and gated as owner decision Q1.

## 1. Problem and evidence

**1.1 The core exhibit: a false soundness claim shipped, and stayed shipped, until a probe found it.** Rationale §5.1's v0.180 record, in its own words: what forced the consume-site freeze was the case with *"no runtime backstop of any kind"* — an object moved to another domain stays alive, no disposed flag is ever set, and a surviving pre-transfer borrow is a live data race, *"probe-confirmed, 2026-07 review — §30.14's isolation claim was false without the freeze."* The claim ("no mutable state is ever reachable from two domains") had been normative since v0.133; the hole was expressible in three lines (`var b := x; Domain.Spawn(W, move x);` then use `b` — v0.246 marker spelling); nothing in the process — spec review, compiler tests, the guides' re-traces — was *shaped* to find it, because each checks text or code against text, and this was a false *theorem*. It fell to an adversarial probe, years of revisions late. A mechanized isolation theorem fails to prove the moment the machine admits that program; the counterexample generator hands back the exact three-line witness.

**1.2 The record generalizes.** The run logs are a chronicle of ownership-soundness holes found empirically, after shipping, by adversarial discovery sweeps: the owned-field-move double-free matrix, the announced-kill join refinements (v0.184), the frame-carried-capture dangle that v0.196 closed (rationale §5.25 — an iterator frame over a capturing callback *"passed the frame-return check by the letter"* while the same borrow as a plain argument was rejected: a statically certain dangle, guarded by nothing), the drop-on-overwrite/borrow interaction (#1465, #2010/#2016). Each was a *consequence-interaction* bug — two individually sound rules composing unsoundly — which is precisely the class a model catches (the theorem quantifies over all compositions) and case-by-case review does not.

**1.3 Why this is tractable now.** Three things changed. (a) **0046 exists**: the kernel is nine stated principles plus fourteen recorded axioms, with a 48-row mapping table whose DERIVED rows are one-line proof sketches — the model's definitions, lemma list, and coverage oracle are already drafted, which is most of the specification work a mechanization project normally spends its first year on. (b) The chapters are the most stable they have ever been (0046 §1.4): modeling churning text is wasted work; modeling settled text is the cheapest it will ever be. (c) AI labor changes the cost model: mechanization is lemma-tree work — wide, decomposable, verifiable at every node — which is exactly the shape this project's parallel-agent process is unusually good at.

**1.4 What the model buys, concretely.** (i) *The next v0.180-class hole found before shipping*: a proposed §28/§30 ruling is checked by re-running the theorems over the amended model — a failed proof is a counterexample, delivered at design time. (ii) *A falsifier for 0046's own claims*: the kernel's extensional-equality claims (K5's two-predicate freeze criterion, K2's source taxonomy) become checkable statements rather than reviewed prose; a derivation the model cannot reproduce is a finding against the kernel or the mapping row (0046 §11). (iii) *A second oracle for the compiler*: the validation bridge (§5.3) is a differential test between model and `bezelc` over generated programs — an accept/reject fuzzer with a proven reference, mechanically related to the adversarial sweeps that found the 1.2 list, but exhaustive-by-construction over the fragment it covers. (iv) *The trust anchor the Tier-1 program trades in* (P1–P5 all ultimately appeal to "the checker is right").

## 2. The design: scope, in layers, with the reduction arguments

The model does not cover surface Bezel; it covers a core calculus, **BZ**, staged in layers. Every exclusion below carries its reduction argument — the reason the excluded feature adds no *ownership-theoretic* content to the layer's theorems, or the layer where it enters.

| Layer | Contents | Theorems | Stage |
|---|---|---|---|
| **1 (v1)** | First-order monomorphic core: locals, owned/value fields, owned/borrowed/moved-from/maybe-consumed states, `move`, marked transfers, scope exit with disposal, exception edges with edge parity, drop-on-overwrite, announced kills (`Dispose`/discard/`throw`), the consume-site freeze, `using var` pins, first-order calls with fully annotated signatures | T1, T2 | S1 |
| **2** | Domains, sendability (transfer-closure), `Domain.Spawn` (fire-and-forget form), channels (send/receive/close) | T3 + T3ₓ | S2 |
| **2.5** | Borrow-holding storage: borrow-bearing records, plain (borrowing) class-typed fields, collections and the two families | T3 conditional form | post-S2 |
| **3** | Frames-as-values: tasks, iterators, lazy queries, closures/capture sets, Pattern A/B, §28.14 admissions | T4 | post-S2 |

**2.1 Layer 1 (v1): the first-order core.** Everything K1, K3, K4, K5, K7, K8 (degenerate: see below), and K9 say about *locals, fields, and scopes* — the fragment in which every historical exactly-once-disposal bug in the 1.2 list lives. Calls are first-order and monomorphic with **fully annotated signatures**: `owned` parameters, `owned` returns, and source-annotated borrow returns (§3.1) — the calculus-level normal form of K2.4's signature-level re-establishment. Full expressions are degenerate in v1 (the calculus is A-normalized: every producer is bound or consumed immediately, so K8's boundary coincides with the statement), which is a deliberate simplification recorded in the disposition register — the K8 content that matters to T1 (temporaries join the cleanup stack, per-evaluation boundaries) is carried by the A-normal form's explicit temporaries.

**2.2 Layer 2: domains.** The L1 machine replicated per domain, interleaved, plus the three crossing constructs. Sendability is modeled as transfer-closure over the calculus's class table (§30.14 rules 2–3). The calculus has no subtyping, so §30.14 rule 1 (sealing) has no model content: sealing is the *surface mechanism* that makes the real language match the model's closed-world class table, and this is recorded as a modeled assumption tied to rule 1, not proven.

**2.3 Excluded from v1, each with its reason.**

- **Generics.** A monomorphic calculus suffices for every ownership question: `owned` is inadmissible on generic type arguments (axiom A14 — owning semantics live in named types), per-instantiation erasure (K1.6) means an instantiated body is ownership-equivalent to a monomorphic body with the substituted classifications, and the one genuinely generic ownership rule — v0.194's bare-type-parameter source sets — is surface elaboration of K2.4, which the calculus models *directly* via source-annotated returns (§3.1). Nothing in T1–T3 quantifies over type structure.
- **Closures and async frames → Layer 3.** Frames-as-values (K6) is a real second half of the model — carried source sets, settlement bounds, capture-set provenance (rationale §5.25) — and T4 is its theorem. It is staged, not skipped: the L1 machine is designed so a frame is *one new value form* carrying a source set, not a redesign (K6 states frames as ordinarily-classified values; the L1 heap and cleanup stack are already the right shape). Attempting it in v1 would couple T1's proof to the hardest open modeling questions (suspension points, per-domain continuation queues) for no gain on T1/T2.
- **Borrow-bearing records and borrow-holding storage → Layer 2.5.** These are K2.2 source-set *transport* (a record local carries its sources; the freeze premise already ranges over source sets, so 2.5 changes no rule shape) — but plain class-typed fields and collection-held borrows are exactly the **residual-trust admissions** (K4.3): the surface where isolation is trust-conditioned rather than checked. Including them in v1 would force T3 into its conditional form from the start (§4.1); excluding them makes v1+L2's T3 *unconditional on its fragment*, which is both the stronger statement and the honest decomposition — the trust-conditioned extension is its own result.
- **Collections → Layer 2.5.** Per-slot states are K1 applied per slot (0046 row 35): an owning collection is, for the disposal theorems, a family of owned fields with an index abstraction, and the model's owned fields already exercise that machinery (K9's per-slot conservative tracking included). What collections genuinely add — the two-family split (A9), the transit exception (A8), the borrowing-store trust rule — is the same trust surface as borrow-bearing records, and enters with them.
- **Statics.** K2's one distinguished always-live source; trivial for every v1 theorem (never disposed before process exit, §22.13). Recorded exclusion; source sets in v1 range over locals, parameters, and field paths.
- **FFI: axiomatized boundary, permanently.** §32.10's posture is documented trust in place of compiler verification; the model states the boundary as an axiom (foreign calls preserve the machine invariants) and proves nothing about it — the honest formalization of what the spec itself promises, in K4.3's own "recorded trust" vocabulary. Not staged; this is the end state.
- **Nullability and definite assignment.** Deliberately out (0046 §2.3 draws the same line): the calculus carries `null` and the uninitialized state only as far as K1's bookkeeping needs them (the polymorphic-null producer row, K7.5 absorption); §7's narrowing machinery is a separate system with its own (worthwhile, unproposed) model.

## 3. The design: the calculus, sketched

This section demonstrates the calculus is writable; stage S0 (§8) hardens it to the full rule set (~40 rules) as a working paper in `formal/calculus/`. Notation: `CT` the class table; states range over `own | brw(S) | moved | disposed | maybe | uninit` with `S` a finite source set; the K/A tags cite 0046 §2.

### 3.1 Syntax (the BNF: nine nonterminals, ~30 alternatives)

```
Prog   ::= Class* Method* Stmt                    -- Stmt is the main body
Class  ::= "class" C "{" Field* "}"
Field  ::= "owned" f ":" C ";"                    -- owned reference field (K9)
         | "val" f ";"                            -- value field (no classification, K1.6)
Method ::= "method" m "(" Param* ")" ":" Ret "{" Stmt "}"
Param  ::= "owned" x ":" C | x ":" C | "val" x    -- owned transfer | borrow | value
Ret    ::= "owned" C                              -- transfer to caller (K3.2)
         | C "@" "{" x* "}"                       -- borrow; declared source set over
                                                  --   params (K2.4 normal form; §2.3)
         | "unit"
Stmt   ::= "skip" | Stmt ";" Stmt
         | "var" x ":=" Rhs | "using" x ":=" Rhs  -- binding | pinned binding (A5)
         | x ":=" Rhs                             -- reassignment (drop-on-overwrite, K3.11)
         | x "." f ":=" Rhs                       -- field store (K3.1 / K3.11)
         | "scope" Stmt "end"
         | "if" "*" "then" Stmt "else" Stmt "end" -- nondeterministic branch (§3.4 note)
         | "while" "*" "do" Stmt "end"
         | "dispose" x | "discard" Rhs            -- announced kills (K4.2)
         | "throw" Rhs | "try" Stmt "catch" x Stmt "end"
         | [ "var" x ":=" ] "call" m "(" Arg* ")"
Rhs    ::= "new" C | "null" | x                   -- fresh | null | unmarked read (borrow)
         | "move" x                               -- marked transfer (K3.4)
         | "move" x "." f                         -- owned field take (K9.1)
         | x "." f                                -- field read (borrow, source {x})
Arg    ::= x | "move" x | "move" x "." f | "new" C
```

Two decided simplifications, both recorded in the disposition register: branch and loop conditions are **nondeterministic** (`*`) — data computation is irrelevant to every theorem, and `*` covers all path behaviors, which is strictly more adversarial than any concrete predicate; and the grammar is A-normal (§2.1). Axiom **A7** is modeled by omission in its exact live scope (v0.209-narrowed; v0.246 re-grounded the register row semantically, positions unchanged): its two rejected positions — the `??`-operand and the operator-operand — do not exist in this grammar. The grammar's lack of arm/operand *expression positions in general* is a consequence of A-normalization, not of A7 — arm positions carry the K3.4 marker in the surface since v0.209 (S0 design note 2).

### 3.2 Machine state

```
sigma = (H, D, U)
H : Loc -> <C, rho, flag>        -- heap: class, field map rho : f -> Loc|null|MOVED,
                                 --   flag in {live, disposed}; entries are never
                                 --   removed (the flag persists: §28.12's logical/
                                 --   physical split, abstracted — K4.1's backstop)
D : stack of frames              -- each frame: bindings x -> (Loc|null|uninit, dtag),
                                 --   dtag in {own, brw, moved, disposed}, plus a
                                 --   cleanup list in establishment order (K3.15)
U : unwind context               -- try-frames; carries the in-flight thrown Loc
```

Disposal is a meta-operation `DISPOSE(l)`: if `H(l).flag = live`, set `disposed`, **emit event `disp(l)`**, then `DISPOSE` each owned field of `l` in reverse declaration order (§28.4 cascade, K9); if already disposed, no-op (idempotence, K4.1). The event trace is the instrumentation T1 counts and the bridge's level-2 oracle replays (§5.3).

### 3.3 Judgment forms

```
Gamma |- r : C [q] |> Gamma'      -- Rhs classification: q in {own, brw(S)};
                                  --   may update Gamma (a move marks its source)
Gamma |- s -| Gamma'              -- statements: flow-sensitive state transformer,
                                  --   the static checker's shape (syntax-directed;
                                  --   mirrors the compiler's per-body analysis)
<s, sigma> --> <s', sigma'>       -- small-step dynamics; distinguished terminal
                                  --   configurations DONE and FAULT(DisposedAccess)
```

The statement judgment is parameterized by a standard backward liveness analysis `L` over the enclosing body (computed by a pre-pass, exactly as the compiler computes it; K5.5's body-local-by-construction clause is why this is well-defined). `frozen(x, Gamma, L)` abbreviates: ∃y live-out at this point with `Gamma(y) = brw(S)` and `x ∈ S`.

### 3.4 Selected rules

Producers and reads (K1.5, K3.13):

```
        C in CT
T-New   ---------------------------------
        Gamma |- new C : C [own] |> Gamma

        Gamma(x) = own C
T-Read  --------------------------------------    -- unmarked read never consumes
        Gamma |- x : C [brw {x}] |> Gamma         --   (K3.13); source is the binding

        Gamma(x) in {own C, brw(S) C}     f an owned field of C
T-FRead ---------------------------------------------------------
        Gamma |- x.f : C_f [brw {x}] |> Gamma     -- storage reads lend (K1.5, K9.1);
                                                  --   val-field reads carry no
                                                  --   classification (K1.6)
```

The marked transfer, and the freeze as its guard (K3.4, K5):

```
        Gamma(x) = own C        not frozen(x, Gamma, L)
T-Move  -----------------------------------------------
        Gamma |- move x : C [own] |> Gamma[x := moved]

        Gamma(x) = own C        frozen(x, Gamma, L)
T-Frz   -----------------------------------------------    -- the v0.180 rule: a
        Gamma |- move x  REJECT(K5)                        --   transferring kill is
                                                           --   neither announced nor
                                                           --   flag-backstopped
```

The announced kill, deliberately *without* a freeze premise (K3.10, K4.2 — unfrozen ⟺ announced ∧ backstopped; the call is the announcement, the flag is the backstop):

```
        Gamma(x) = own C
T-Disp  ----------------------------------------------
        Gamma |- dispose x -| Gamma[x := disposed]
```

A later straight-line read of a `disposed` binding is **accept-with-warning** (§28.2's mandatory warning; the checker's verdict domain is three-valued, §5.3); a read of `moved` or `maybe` is REJECT. Drop-on-overwrite (K3.11, K5.2 — disposing but not keyword-visible, hence frozen):

```
        Gamma(x) = own C     Gamma |- r : C [own] |> Gamma'     not frozen(x, Gamma', L)
T-Asgn  ---------------------------------------------------------------------------------
        Gamma |- x := r -| Gamma'[x := own]
        -- dynamics: RHS evaluated first, then DISPOSE(old value) (K9.5's RHS-first
        --   discipline, applied to bindings per §28.7); the borrowed-LHS dual admits
        --   only [brw] RHSs (state-match, 0046 row 6), with source-set replacement
```

The join (K7, the meet table written once):

```
        Gamma |- s1 -| G1        Gamma |- s2 -| G2
T-Join  ------------------------------------------------
        Gamma |- if * then s1 else s2 end -| G1 MEET G2

  own    MEET own    = own            brw(S1) MEET brw(S2) = brw(S1 u S2)   (K2.2)
  own    MEET brw(_) = REJECT(K7.3)   own     MEET moved    = maybe         (K7.4)
  own    MEET disposed = maybe        null/uninit absorb per K7.5 / Sec 7.9
  -- while * do s end: s's transformer iterated to fixpoint over MEET; a
  --   back-edge that turns own into maybe rejects any in-loop read (K7.4)
```

Scope exit, with edge parity (K3.14, K3.15):

```
        Gamma |- s -| Gamma'
T-Scope ----------------------------------------------------
        Gamma |- scope s end -| Gamma' \ locals(s)
        -- statically: nothing to check at the brace — affinity means an
        --   undischarged own is disposed, not an error (a must-use WARNING
        --   for plain var, K3.14); dynamically:

E-Scope <scope-exit, (H, F:D, U)> --> <skip, (H', D, U)>
        where H' = DISPOSE each (x -> l, own) in F's cleanup list, REVERSE
        establishment order; for maybe-state locals, per-path (the dtag
        carries the runtime truth).  THE SAME RULE fires during unwinding
        (K3.15 edge parity): an in-flight throw pops frames through E-Scope
        before reaching its catch — one cleanup stack, two edges, no
        separate exceptional machinery to get wrong.
```

Calls: the signature is the announcement, re-established against the actuals (K2.4, K3.3) — the rule that makes spawn/send guarding in Layer 2 an instance rather than a special case:

```
        sig(m) = (owned p1: C1, p2: C2) -> owned C0
        Gamma(a1) = own C1        not frozen(a1, Gamma, L)      -- K3.3 consume,
        Gamma |- a2 : C2 [brw(S2)] |> Gamma                     --   K5-guarded
T-Call  ----------------------------------------------------------------------
        Gamma |- var y := call m(move a1, a2) -| Gamma[a1 := moved][y := own]
        -- an owned-return call is a fresh producer at the caller (K1.5); had
        --   m declared C0 @ {p2}, y would instead be brw(S2) — the declared
        --   source set substituted with the actuals' sources (K2.4); a
        --   Layer-2 spawn/send is T-Call with a transfer-closure premise
```

`throw`, the keyword-announced consume (K3.8, K3.13's announcement principle, K4.2 — the freeze premise is deliberately absent, K5.3):

```
        Gamma(x) = own C          -- position-announced: no marker, no freeze
T-Throw -----------------------------------------------------------
        Gamma |- throw x -| unreachable
        -- dynamics: the in-flight Loc rides the unwind context U; frames pop
        --   through E-Scope (edge parity); the catch binding is a borrow of
        --   the in-flight exception (spec Sec 31.1 Exception ownership; the pattern-binding analogy is Sec 19.3 / 0046 Sec 6.6), and

E-CatchDone   <catch-arm-complete(l), sigma> --> DISPOSE(l), then continue
              -- Sec 31.1: the machinery disposes after the matching arm
              --   completes; T1 counts this disposal like any other
```

The owned-field take and the restoration obligation (K9.1, K9.2):

```
        Gamma(x) = own C    f an owned field of C    slot(x.f) = intact
T-MvF   -------------------------------------------------------------------
        Gamma |- move x.f : C_f [own] |> Gamma[slot(x.f) := moved]
        -- side judgment RESTORE: on every path from here to the method's
        --   exit — normal, throw, and every exception edge of an intervening
        --   throwable expression — slot(x.f) is reassigned before exit
        --   (Sec 28.8.1); destructor bodies exempt (K9.3); the v1 calculus
        --   has no statics, so K9.4's static ineligibility is vacuous
```

Checked-mode dynamics (K4.1 — the rule T2 is *about*):

```
        H(l).flag = disposed      the redex reads or writes a field of l,
                                  or calls through l
E-UAK   ---------------------------------------------------------------
        <s, sigma> --> FAULT(DisposedAccess)      -- the ONLY applicable
                                                  --   transition: determinism
                                                  --   is the theorem's content
```

### 3.5 Layer 2 delta (sketch)

Configurations become `(H, {D_1..D_n}, Chans)`: one binding/cleanup stack per domain, one shared heap partitioned by a per-domain root reachability map, channels as distinguished heap nodes exempt from the partition (§30.15: the only mutable objects reachable from more than one domain). New statements: `spawn m(move x)` (capture-free by construction — the calculus's methods have no environment; §30.14's capture-free entry rule is the surface mechanism matching this, recorded like sealing in §2.2), `send c (move x)`, `var y := recv c`, with `send`/spawn-argument positions being K3.3 consume positions (hence guarded by T-Frz — the load-bearing premise). Sendability: the argument's class must be transfer-closed over `CT` (all fields `owned`-sendable or `val`, coinductively — §30.14 rules 2–3). Channel close disposes undelivered items in reverse insertion order (§30.15), counted by T1's trace. Steps interleave domains arbitrarily at statement granularity; T3 is an invariant over all interleavings.

## 4. The theorems

### 4.1 Statements

Let `check : Prog → {accept, accept-warn(W), reject(R)}` be the checker; "well-typed" means `check(P) ≠ reject`. Traces are maximal step sequences; `allocs(tr)` the locations minted by `T-New` steps; `disp(l)` the disposal events of §3.2.

- **T1 — exactly-once disposal.** For every well-typed `P` and every **complete** trace `tr` — terminating in `DONE` or in a top-level uncaught `FAULT`/throw, the unwind edge running the same `E-Scope` cleanups — every `l ∈ allocs(tr)` has **exactly one** `disp(l)` event; and for every finite prefix of *any* trace, **at most one**. (The prefix form is the claim for nonterminating programs; the flag heap makes "exactly once" a transition count, with the idempotent no-op path excluded by construction.) Exceptional paths are not a special case in the proof: edge parity (one cleanup stack, §3.4 E-Scope) makes them the same induction.
- **T2 — checked-mode determinism of use-after-kill.** The checked machine has **no undefined states**: every reachable configuration of a well-typed program steps, is `DONE`, or is a named `FAULT` (progress — in particular, reads of `moved`/`maybe` bindings are statically rejected, so no stuck read exists); and every access through any alias to a location whose disposal event has occurred takes the `E-UAK` step to `FAULT(DisposedAccess)` — it is the *only* enabled transition for that redex, on every execution reaching it (determinism modulo the `*`-branch oracle; in Layer 2, under every interleaving). This is §28.2's checked mode as a theorem: unchecked mode is the same machine with `E-UAK`'s premise never consulted, and T2 is exactly the claim that the two modes agree on every program that never reaches an `E-UAK` redex.
- **T3 — domain isolation (Layer 2).** Invariant, at every step of every interleaved trace of a well-typed L2 program: for every heap location `l` that is mutable and not a channel core, `l` is reachable from at most one domain's roots. Corollary (the §30.13 guarantee the ownership chapter leans on): no two domains ever access the same non-channel location concurrently. On the v1+L2 fragment this is **unconditional** — the fragment has no residual-trust admissions (§2.3), so every borrow holder is a local and the freeze premise (T-Frz) sees it. The Layer-2.5 extension states the honest conditional form: well-typed **and** obligation-honoring (a semantic predicate formalizing K4.3's recorded trust clauses — no borrowed-from object transferred to, or made domain-reachable from, another domain) implies isolation; that conditionality is not a weakness of the model but the *content* of the spec's own posture, stated in §30.14 and 0046 K4.3.
- **T3ₓ — freeze necessity (the mechanized regression).** In the L2 system with T-Frz's premise deleted (T-Move without the `not frozen` side condition), there exists a well-typed program and an interleaving reaching a state violating T3's invariant — the witness being the three-line v0.180 probe of §1.1. This lemma is cheap (one `exists`, one program, one trace) and is the proposal's thesis in miniature: the model *contains* the historical soundness hole as a permanently re-checked counterexample, so no future "simplification" of the freeze can silently reopen it.
- **T4 — frame-residence soundness (staged, Layer 3).** For every well-typed L3 program: (a) *adequacy* — K6's carried-source computation (receiver + plain reference-typed arguments + capture-set contributions, transitively, per rationale §5.25's provenance table; re-established per K6.2) over-approximates the set of sources any execution of the frame dereferences; (b) *soundness under discharge* — an execution in which every frame settles (or is disposed) before any of its carried sources' scopes exit never reaches `E-UAK` through a frame-held borrow; and (c) the abandonment idioms fault deterministically per T2 rather than reading freed memory. Stated now so the L1 machine is built with frames in mind (§2.3); proven at Layer 3.

### 4.2 Alignment with 0046 §11's theorem map

The T-numbering above is frozen by this proposal; each theorem cites its 0046 §11 row, and the two documents' numberings never diverge (0046 §11 lists the rows unnumbered, in a different order — the correspondence is recorded here once):

| This proposal | 0046 §11 row | Kernel principles exercised (0046's column, adopted) | Carried assumptions (0046's column) |
|---|---|---|---|
| T1 | "exactly-once disposal on every path" | K1, K3, K7, K9 | A12, A13 |
| T2 | "no silent use-after-consume" | K4, K5, K7 | A7, A8 |
| T3 (+T3ₓ) | "domain isolation" | outside the kernel (0046 §2.3) — modeled from §30.13–§30.15 directly, with K4.3's domain clause as the interface obligation | — |
| T4 | "frame-residence soundness (rationale §5.25)" | K2, K6, K8 | A10, A11 |

One alignment note, stated rather than fudged: 0046's carried-assumption columns name the axioms the *full-language* theorem must carry. Under this proposal's staging, each assumption attaches at the layer that admits its feature — v1's T1 carries neither A12 (Pattern A chaining timing — Layer 3) nor A13 (move-only inline arrays — excluded), so v1's T1 is *exact on its fragment*, and the disposition register below is the mechanism that keeps this honest rather than quietly narrower.

### 4.3 The axiom-disposition register

The model tracks 0046's register A1–A14 as a per-axiom **disposition**, maintained in `formal/STATUS.md` and mirrored as a Lean definition (`Bz/Register.lean`) so the build fails if the model and the ledger disagree. Dispositions: **MODELED** (the axiom is a definition in the model), **MODELED-BY-OMISSION** (the calculus's grammar or class table embodies it), **ASSUMED** (stated as a hypothesis of a theorem), **EXCLUDED(layer)** (its feature is out of the current layer; attaches at the named layer). At v1:

| Axiom | Disposition at v1 | Note |
|---|---|---|
| A1 (factory heuristic) | EXCLUDED — modeled world is fully annotated | The calculus's signatures are explicit (§3.1), i.e. v1 models the **post-0048 world**; see §7 for the tracking consequence when 0048 lands. |
| A2, A3, A6, A10, A11, A12 | EXCLUDED(L3) | Task-family/calls-mint, two-container async, discard ban, certifications, §28.14 forms, chaining timing — all frame machinery. |
| A4 (accessor lend-only) | EXCLUDED — no accessors; K1.5 subsumes | Field reads lend in the model; the accessor face is surface. |
| A5 (`using var` pin) | MODELED | The pinned-binding form (`using x :=`), non-reassignable. |
| A7 (no `??`-operand / operator-operand marker) | MODELED-BY-OMISSION | The two rejected positions (A7's live scope since v0.209; row re-grounded semantically at v0.246) do not exist in §3.1's grammar; the general absence of arm/operand expression positions is A-normalization, not A7 (S0 design note 2). |
| A8, A9 (transit, two families) | EXCLUDED(L2.5) | Enter with collections. |
| A13 (inline-array carve) | EXCLUDED — move-only value types out of scope | Which is *why* v1's T1 is exact; the disposition flips to ASSUMED if the feature is ever modeled before #1385/#747 resolve. |
| A14 (no owned generic args) | MODELED-BY-OMISSION | The monomorphic calculus is A14's world; §2.3's generics reduction leans on it. |

This table is 0046 §11's coverage oracle made operational: *"the model's assumption count and the register must agree."* Every register change upstream (an axiom added, reclassified, or tombstoned) forces a matching disposition edit, checked by the ledger discipline of §7.

## 5. The design: methodology

### 5.1 Vehicle: Lean 4

Decided, with reasons (the P7 sketch said "Lean 4 (or equivalent)"): Lean 4 compiles its definitions to native code, so the **checker is directly executable** — the same artifact that appears in the theorems drives the bridge, with no extraction step to trust (Coq would add one; Isabelle's code generation likewise). The model needs no mathematical library (finite maps, lists, and induction — core `Std` suffices; **no mathlib dependency**, deliberately, to keep the toolchain one pinned binary via `lean-toolchain` + `lake`). Agent familiarity and the ecosystem's momentum are real but secondary tiebreakers. The choice is an artifact-scoped dependency adoption and is gated as **Q1** (§12) under CLAUDE.md's dependency policy — see §5.4.

### 5.2 Two presentations, one agreement theorem

The model carries the static semantics twice, on purpose: a **declarative** system (the Γ ⊢ s ⊣ Γ' rules of §3.4, one Lean file per kernel principle, `Bz/Kernel/K1.lean`…`K9.lean`, each definition's docstring citing its K/A id and spec section) and a **computable, syntax-directed checker** `check` mirroring the compiler's shape (per-body flow analysis over a liveness pre-pass). The agreement theorem — `check P ≠ reject ↔ derivable(P)`, and warning-set agreement — is proven once and keeps them honest. The division of labor mirrors the thing being modeled: the declarative system is the spec-facing artifact (readable rule-for-rule against 0046 §2), the checker is the bridge- and compiler-facing artifact, and their agreement is the model's internal version of the spec-vs-compiler fidelity question. Each DERIVED row of 0046 §4 that falls in the current layer becomes a named lemma whose proof is expected to be the row's one-line derivation, elaborated; **a row whose lemma needs materially more than its one-line sketch is a finding against the kernel or the row, filed** (0046 §11 states the same rule from its side).

### 5.3 The validation bridge (the differential oracle, and a stage gate)

The bridge answers the question every model faces — *is this the same language the compiler implements?* — mechanically, not by inspection. Three levels, each a concrete deliverable:

1. **Verdict differential (S1 gate).** A pretty-printer `bzlprint : Prog → .bzl` emits each calculus program as a real, self-contained Bezel module (classes with `owned` fields, static methods on a carrier class, the main body as `public static method Main()` per §33.2; `*`-branches specialized to literals per enumerated path — §3.4's oracle resolved at generation time). The harness runs `check` and `build/bezelc` over the same program and compares three-valued verdicts: accept / accept-with-warnings / reject. Diagnostic *identity* is compared at rule-family granularity (freeze-class, join-class, restoration-class, …), not message text (Q3). Example — the calculus program `var x := new R; var b := x; var y := move x; use b` prints as:

   ```bezel
   module Probe;

   public class R
     public constructor() end constructor;
   end R;

   public class P0001
     public static method Main()
       var x := new R();
       var b := x;               // unmarked read: borrow of x (§28.3)
       var y := move x;          // marked transfer while b is live
       Console.WriteLine(b);     // keeps b live past the consume
     end Main;
   end P0001;
   ```

   Model verdict: `reject(K5)`. Expected `bezelc` verdict: the §28.3 consume-site-freeze diagnostic. A mismatch in either direction is a filed finding (§7). Every current-surface construct in emitted programs is verifiable against the stdlib doc (`Console.WriteLine`, stdlib §2.1) — the printer's vocabulary is deliberately tiny so the cardinal no-invented-APIs rule is enforceable by inspection of one function.
2. **Trace differential.** For model-accepted programs, the printer instruments each class with a `destructor` that writes a disposal marker (`Console.WriteLine` of an identity assigned at construction — no stdlib surface beyond §2.1 needed), and the harness compares the process's emitted disposal trace against the model's `disp(·)` event trace — **order included**, since RHS-first drop-on-overwrite and reverse-establishment-order scope exit are deterministic and normative. Calculus `scope s end` prints as the surface's bare block, `do … end do;` (Appendix A's `DoEndBlock`). Worked example — the calculus program

   ```
   scope var a := new R;                        -- R#1
         scope using b := new R;                -- R#2
               a := new R;                      -- R#3; drop-on-overwrite disposes R#1
         end
   end
   ```

   has model trace `disp(R#1), disp(R#2), disp(R#3)` (K3.11 RHS-first; then the inner pin at its scope end; then `a`'s final value at the outer), and prints as:

   ```bezel
   module Probe;

   public class R
     private _id: int;
     public constructor(id: int)
       _id := id;
     end constructor;
     destructor
       Console.WriteLine($"disp {_id}");
     end destructor;
   end R;

   public class P0002
     public static method Main()
       var a := new R(1);
       do
         using var b := new R(2);
         a := new R(3);            // drop-on-overwrite: "disp 1" emitted here
       end do;                     // pin's scope end: "disp 2"
     end Main;                     // a's scope end: "disp 3"
   end P0002;
   ```

   The compiled program must print `disp 1`, `disp 2`, `disp 3` — anything else is a filed finding against compiler or model. (Both sides' *static* verdict here is accept-with-warning — `a`'s implicit scope-end drop draws K3.14's must-use lint — which is exactly the three-valued agreement level 1 checks.) Checked-mode fault programs are compared on fault identity (`DisposedAccessException` reaching the §33.3 uncaught-exception path, exit code 70). This is T1/T2 checked *against the real compiler*, program by program.
3. **Generation.** Bounded-exhaustive enumeration of calculus programs up to size n (the L1 grammar is small; the state space at n≈stmt-depth-4 is large enough to cover every pairwise rule interaction in the §1.2 bug class) plus targeted mutation around the freeze/join/restoration rules. Run under `dev_heavy.sh probe` discipline with outputs in the scratch tree, batched into the harness like a conformance sweep; adjacent to, but independent of, proposal 0053's in-language property testing (which tests Bezel programs; this generates them).

**As a stage gate:** S1 does not close until the bridge runs level 1 over (a) a transcribed corpus of every §28.2–§28.8.1 and Guide 01 example inside the v1 fragment, with expected verdicts, and (b) ≥10⁴ generated programs — with **zero unexplained divergences** (every divergence filed and either resolved or recorded with its issue number in `formal/STATUS.md`). The bridge is what makes the model a compiler-testing asset on day one rather than a parallel universe.

### 5.4 Repository placement, and the dependency policy, addressed explicitly

CLAUDE.md: *"Adoption of new dependencies. Dependencies are debt; adding one is a project-level decision."* This proposal's answer, in full:

- **The model is a verification artifact, not a build input.** It lives in a new top-level `formal/` tree: `formal/calculus/` (the S0 working paper), `formal/model/` (the `lake` project: `lean-toolchain` pin, `Bz/Syntax.lean`, `Bz/Machine.lean`, `Bz/Statics.lean`, `Bz/Checker.lean`, `Bz/Kernel/`, `Bz/Theorems/`, `Bz/Register.lean`), `formal/bridge/` (printer, harness script, corpus), `formal/STATUS.md` (the ledger, §7), `formal/README.md`.
- **`bezelc` never touches it.** No CMake target references `formal/`; no compiler source includes anything from it; building, testing, and gating the compiler on a machine with no Lean toolchain is unaffected — the same relationship `docs/` has to the build today. The dependency being adopted is scoped to the artifact, and *that adoption is still an owner decision* (Q1), not something this proposal assumes.
- **CI is advisory and optional.** A separate workflow lane (alongside `.github/workflows/ci.yml`) that (a) builds the Lean project (`lake build` — the proofs *are* the build), and (b) runs the bridge corpus against the in-tree `bezelc`. It is non-blocking by policy: a red `formal` lane files issues, never gates merges, and never enters `scripts/gate.sh`'s path. Cadence per Q7 (recommended: on changes under `formal/` or `docs/`, plus nightly).
- **Toolchain pinning.** `lean-toolchain` pins the exact Lean release; the lane installs via `elan` at that pin. No system-package coupling, no version drift into the compiler's Fedora/clang/LLVM story.

## 6. Interactions

1. **Proposal 0046 — the source of truth for K/A.** This proposal adopts K1–K9/A1–A14 verbatim and consumes 0046 §11's bullets as design input (definitions from K's; DERIVED rows as lemmas; axioms as explicit assumptions; the acceptance audit as the coverage spec; the theorem map as §4.2). Sequencing: S0 may begin against 0046's *draft* numbering immediately, but S1 does not start until 0046's T1 lands and freezes the numbering (or the owner accepts the draft-pin risk — Q2 of §12 records the choice); a renumbering after S1 starts would invalidate every docstring citation.
2. **Proposal 0048 — the first live register change.** 0048 deletes A1. The model's v1 world is already post-A1 (fully annotated signatures, §4.3), so the model change is *ledger-only*: A1's disposition row gains the tombstone (`deleted at v0.NNN per 0048`), `Bz/Register.lean` updates to match, and the model's revision pin advances. This is the worked example of §7's tracking discipline, and it is deliberately cheap — the layering chose the annotated world partly so that the scheduled register change would be.
3. **Spec §30.13–§30.15 as direct source text.** For T3 the kernel is not the source (0046 §2.3 excludes domains); the model takes the three sections directly, with K4.3's domain clause as the interface obligation — exactly the seam 0046 §11 states. If the domains half ever gets its own kernel consolidation, the model re-points at it; until then, `formal/calculus/` carries the L2 rules with §30.13–§30.15 citations.
4. **The conformance corpus (`tests/conformance/`).** The bridge's transcribed-example corpus is maintained in `formal/bridge/corpus/` in the conformance harness's spirit (expected-verdict manifest), but separately — conformance cases are spec-example-shaped, bridge cases are calculus-image-shaped. Where they overlap (a spec example inside the v1 fragment), the bridge corpus cites the conformance case id rather than duplicating.
5. **P12 (derivation-carrying diagnostics).** The model's declarative rules are the derivations P12 wants to surface; a P12 proposal can cite `Bz/Kernel/*` lemma names as the canonical derivation ids. No coupling now.
6. **P3 (contract static tier) and P5 (deterministic replay).** Consumers of trust, not participants; noted only because the future-directions sequencing names P6→P7 as their prerequisite trust anchor.
7. **The compiler.** Zero compiler work in any stage. Bridge-discovered divergences are *outputs* (filed issues, exactly like 0046's audit divergences), assigned to whichever side is wrong by the ordinary spec-first discipline.

## 7. Migration and compatibility

There is no migration in the program-compatibility sense (no spec text, no diagnostics, no programs change). The section's content is the **model-maintenance contract** — what "the model is alive" means after S3:

- **Versioning.** The model pins the spec revision it verifies: `formal/STATUS.md` header carries `model M-k ⇄ spec v0.NNN`, the layer coverage, the axiom-disposition table (§4.3), and the exclusions ledger (every §28/§30 rule outside the modeled fragment, with its layer or its reason). A docs revision that touches §28/§30 either ships a model update (M-k+1, same landing train or a fast-follow) or adds one line to the exclusions ledger — the S3 process rule, added to `spec-change-process.md` §9 as: *"— [ ] If the change touches §28/§30: update `formal/` (model + STATUS.md) or record the exclusion in `formal/STATUS.md` with reasons."* This composes with, and does not duplicate, 0046's own checklist line (kernel classification): 0046's line makes a ruling say *what it is* (derivation/axiom); this line makes it say *where the model stands on it*.
- **Register tracking.** The 0046 axiom register is the model's assumption interface. Register changes upstream (0048's A1 deletion; future lifts of A7/A11/A13; new axioms from future rulings) are tracked by disposition edits in lockstep, and `Bz/Register.lean`'s mirror makes a forgotten edit a build failure rather than silent drift. New axioms default to EXCLUDED-with-reason until modeled — the ledger never claims coverage it does not have.
- **Divergence protocol.** A bridge or proof divergence is **a filed issue, never a local fix** — in either direction. The model does not get "corrected" to match the compiler (that would launder an implementation bug into the reference), and the spec does not get amended from inside `formal/` (out of scope for autonomous work per CLAUDE.md; the freeze precedent shows the full path: probe → issue → ruling-class spec change → model update after the ruling). While an issue is open, `STATUS.md` lists it under *known divergences*, and the affected theorem, if any, carries an explicit `sorry`-free weakened hypothesis or is temporarily demoted in the ledger — the model never ships a silent `sorry`.
- **Staleness posture.** If maintenance lapses (the honest failure mode of every verification artifact), the pinned revision keeps the artifact meaningful: "verified against v0.NNN" is a permanent true statement, the exclusions ledger says exactly what it covered, and catch-up is re-running the S1 gate against the current spec. The one thing the design refuses is an unpinned model drifting into aspirational status.

## 8. Staging and effort

| Stage | Content | Exit gate | Size |
|---|---|---|---|
| **S0** | The paper calculus: `formal/calculus/bz.md` — full L1+L2 rule set (~40 rules), each rule K/A-tagged, every 0046 §4 row inside the fragment mapped to its rule or lemma; the §4.1 theorem statements finalized against it | Adversarial critique pass (the 0046 §9 style: hunt the killer counterexample per rule); owner sign-off on layer boundaries (Q2) | **S–M** — the kernel is written; S0 is elaboration, not invention. Days, parallelizable rule-by-rule. |
| **S1** | Lean: L1 syntax, machine, declarative statics, checker, agreement theorem, **T1 + T2**; bridge levels 1–3 over L1; the S1 gate of §5.3 | All T1/T2 proofs `sorry`-free at the pinned revision; bridge gate green (zero unexplained divergences); `STATUS.md` ledger complete | **L** — the long pole. Lemma-tree work: wide and AI-parallelizable (per-rule preservation lemmas, per-construct progress cases), but proof debugging is serial-cognition-bound at the joins (the K7 meet and E-Scope edge parity are where the induction gets hard). Estimate: 2–4 focused runs. |
| **S2** | L2 domains/channels; **T3 + T3ₓ**; bridge extended with spawn/send programs (verdict level; trace level per-domain) | T3 unconditional on the fragment; T3ₓ's witness is the v0.180 probe shape | **M** — the L1 machine replicated + one invariant; the interleaving quantifier is standard. 1–2 runs. |
| **S3** | The process rule (checklist line, §7) + the advisory CI lane (§5.4) + `formal/README.md` | Owner confirms rule text (Q4) and CI cadence (Q7) | **S** |
| *post* | Layer 2.5 (trust-conditional T3), Layer 3 (frames, T4) | Each its own gate, same pattern | L each; scheduled by a future run, not this proposal |

**S1's lemma decomposition** (the parallelization plan, and where the effort actually goes):

| Lemma group | Content | Notes |
|---|---|---|
| Well-formedness | machine invariants: cleanup lists mirror own-tagged bindings; heap flags monotone (live→disposed only) | the induction's working capital |
| Per-rule preservation | each T-rule preserves the Γ⇄machine agreement relation (static state abstracts dynamic tag + heap flag) | one lemma per rule; embarrassingly parallel |
| Progress | accepted programs have no stuck configurations | mostly mechanical case analysis |
| The joins | K7 meet correctness (per-path maybe-tracking abstracts both arms); loop fixpoint | the first hard node |
| Edge parity | E-Scope on unwind = E-Scope on fall-through (one induction covers both T1 edges) | the second hard node; the design of §3.2's single cleanup stack exists to make this a non-lemma |
| T1 assembly | trace counting over preservation + parity + E-CatchDone | |
| T2 assembly | progress + E-UAK exclusivity | |
| Agreement | `check` ⇄ declarative system (§5.2) | independent of the above; can run in parallel |

Sequencing constraints: S0 → S1 → S2 strictly; S3 any time after S1 (the process rule is meaningful once a model exists to update). S1 start is gated on 0046 T1 (or Q2's draft-pin waiver). Nothing here blocks or is blocked by R0 compiler work — the lanes are disjoint by construction (no compiler edits, no shared build).

## 9. Risk register

| Risk | Mitigation |
|---|---|
| **The model models the wrong language** — the classic formalization failure: definitions drift from the spec, proofs verify the drift. | The bridge is the structural answer (a mechanical tie to `bezelc`, gated at S1 with a zero-unexplained-divergences bar); the 0046 mapping table is the second (every rule K/A-tagged, every DERIVED row a named lemma); docstring citations make review a rule-for-rule diff against 0046 §2. |
| **Proof effort balloons** (the historical fate of mechanization projects). | The layer cuts of §2 are the guard: every hard-to-model feature is *out* of v1 with a recorded reason, and S0's critique pass is a cheap kill-point (Q8) before the L-sized commitment. A lemma resisting proof gets triaged like any finding: kernel defect, calculus defect, or genuinely-hard — only the last costs proof labor. |
| **Divergence handling degenerates** — the bridge finds mismatches faster than the ruling process resolves them, and the model starts carrying local patches. | The §7 protocol is absolute (filed issue, never a local fix, no silent `sorry`); `STATUS.md`'s known-divergences list makes backlog visible; the advisory CI posture means an open divergence blocks nothing, so there is no pressure to paper over it. |
| **The artifact goes stale** after S3 (maintenance lapses between runs). | The pinned-revision design (§7): staleness degrades the claim from "current" to "verified at v0.NNN" — a true statement, not a rotting one — and catch-up is a re-run of the S1 gate, not a rebuild. The process-rule checklist line keeps §28/§30 changes from *silently* outrunning the model. |
| **Toolchain risk** — Lean 4 releases move; a pin ages. | `lean-toolchain` + `elan` make the pin exact and reproducible; no mathlib means no dependency cascade; upgrading the pin is an ordinary maintenance commit gated by the advisory lane going green. |
| **The theorems pass but persuade no one** (proofs about a calculus, waved at a compiler). | This is why the bridge is a *gate*, not an appendix: the deliverable is the pair (proved theorems, differential agreement on 10⁴+ programs and the transcribed corpus), and §1.4's payoffs are stated against the pair, not the proofs alone. |

Two failure modes are accepted deliberately: the model cannot find surface-only bugs (parser, sema-outside-§28/§30, codegen — the existing suite's territory, not this artifact's), and Layer-2.5/3 holes stay findable only by the adversarial sweeps until those layers land — this proposal narrows the empirical frontier; it does not abolish it.

## 10. Alternatives considered and declined

- **Prove against the surface language (no calculus).** Declined: the surface grammar (Appendix A) drags in every system the theorems don't need (generics, nullability, accessors, overload resolution), multiplying the mechanization by an order of magnitude for zero additional truth about ownership — and the bridge recovers the connection to the surface *empirically*, which is the appropriate strength (the compiler's parser/sema are exactly the components a surface model would still have to trust).
- **Model checking / bounded exploration instead of theorem proving** (Alloy, TLA+, or a bespoke exhaustive checker). Declined as the *primary* vehicle: bounded tools find bugs but cannot state "every execution of every well-typed program," which is the claim the language actually makes — and the design keeps the bounded tool's entire benefit anyway, as bridge level 3 (bounded-exhaustive generation *with a proven reference checker*), which is strictly stronger than bounded exploration against a hand-written model.
- **Coq or Isabelle instead of Lean 4.** Either would work; Lean is chosen for the no-extraction executable checker (§5.1) and single-binary toolchain pinning. Recorded as a preference with reasons, not a hard technical necessity; Q1 is the ratification point.
- **Embedding the model in the compiler's test suite** (a C++ reimplementation of the checker as an oracle). Declined: a second implementation is a differential tester, not a proof — it finds where two programs disagree but cannot say which is right, and it would live inside the build (violating the isolation this proposal promises). The bridge gets the differential value with the proven artifact as the reference.
- **Making the CI lane blocking.** Declined for the foreseeable horizon: a blocking formal lane couples every docs and compiler merge to a toolchain the project just adopted and to proof maintenance latency; advisory-with-filed-issues gets the signal without the coupling. Revisit only if the lane has run quiet for a long period and the owner wants the ratchet (Q7 notes the option).
- **Modeling domains first** (T3 before T1/T2, since the freeze story is the headline). Declined: T3's machine *contains* the L1 machine — building it first means building both at once under the harder quantifier (all interleavings), and the L1 theorems are where the disposal bug class of §1.2 lives. T3ₓ preserves the headline in S2, where it is cheap.
- **A prose-only "paper calculus" with no mechanization** (stop at S0). Declined as an end state — it reproduces 0046's kernel at a different notation level without the property that pays (machine-checked composition); S0 exists only as the staging step it is.
- **Waiting for the domains half to get its own 0046-style kernel before S2.** Declined: §30.13–§30.15 are short, stable, and edge-enumerated (rationale §12.7's "the striking property is how little there is to say"); modeling from the sections directly is cheaper than a consolidation pass no one has proposed, and 0046 §11 explicitly blesses this seam.

## 11. Recorded divergences from the P7 sketch

1. **Layered scope, not monolithic.** The sketch scopes "the P6 kernel … plus domains, sendability, and channels" as one model; this proposal stages L1 (T1/T2) before L2 (T3) and defers frames (T4) to L3, with the reduction arguments of §2. Reason: the L1 theorems are provable without the interleaving quantifier, and the staging turns the sketch's single L-sized bet into gated increments.
2. **The validation bridge is new.** The sketch's three stages (paper → mechanization → process rule) contain no implementation-contact mechanism; this proposal adds the differential bridge and makes it an S1 *gate*. Reason: a model with no mechanical tie to `bezelc` verifies the spec's text but not the language users run; the bridge is also the near-term bug-finding payoff (§1.4.iii) arriving at S1 rather than never.
3. **T3ₓ (freeze necessity) added as a named lemma.** Not in the sketch. Reason: it mechanizes the exact historical episode that motivates P7 (§1.1) and guards against its reintroduction — the highest evidence-per-line item in the proposal.
4. **Vehicle decided (Lean 4), not "or equivalent."** §5.1's reasons; ratification at Q1.
5. **The axiom-disposition register added.** 0046 §11 requires the model's assumption count to agree with the register; this proposal operationalizes that as the per-axiom disposition table (§4.3) with a Lean-mirrored ledger, extending it to track scheduled register changes (0048) rather than assuming a static register.
6. **The process rule is scoped to §28/§30 with a named recording location** (`formal/STATUS.md`'s exclusions ledger), sharpening the sketch's "updates the model or records why it is out of scope" into an auditable artifact.

## 12. Open questions / owner decisions

1. **Q1 — Adopt Lean 4 as the `formal/` toolchain.** This is a new-dependency adoption under CLAUDE.md's policy — artifact-scoped, never a compiler build input (§5.4), but a project-level decision all the same. Ratify Lean 4, direct an alternative prover, or direct S0-only until a later ratification. **This is the load-bearing gate for S1.**
2. **Q2 — S1 start condition.** Recommended: after 0046 T1 lands (K/A numbering frozen). Alternative: start against 0046's draft numbering now, accepting the re-cite risk if T1's critique renumbers. Owner's sequencing call.
3. **Q3 — Bridge verdict granularity.** Recommended: three-valued polarity (accept / accept-warn / reject) plus rule-*family* identity for rejects; full diagnostic-identity comparison declined as brittle (message wording is compiler surface, P12's lane). Confirm or tighten.
4. **Q4 — The S3 process rule.** Confirm adding the §7 checklist line to `spec-change-process.md` §9 at S3, and its recording location (recommended: one line in the changelog entry *and* the `STATUS.md` ledger; alternative: ledger only).
5. **Q5 — The T3 trust posture.** Ratify the two-step claim: unconditional T3 on the v1+L2 fragment; explicitly *conditional* (obligation-honoring premise formalizing K4.3) at Layer 2.5. The conditional form is the honest formalization of §30.14's own recorded posture, but stating a conditional isolation theorem is an owner-visible choice.
6. **Q6 — Placement.** Recommended: in-repo `formal/` (lockstep versioning with the spec revision is the point; a separate repo reintroduces the drift the ledger exists to prevent). Confirm, or direct a separate repository.
7. **Q7 — CI cadence and posture.** Recommended: advisory lane, triggered on `formal/**` and `docs/**` changes plus nightly; never blocking, never in `gate.sh`. Includes the future option (not requested now) of ratcheting to blocking-for-`docs/`-PRs if the lane proves quiet.
8. **Q8 — Effort authorization.** S1 is an L-sized commitment (2–4 focused runs, §8) with S0's critique pass as the cheap kill-point: confirm proceeding to S1 on S0's gate, or direct S0-then-review.
