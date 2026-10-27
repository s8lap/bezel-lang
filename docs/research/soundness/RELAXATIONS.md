# Can K5.3, K3.13 or K5.2 be relaxed or retired without losing soundness?

*Follow-up to the soundness research lane, 2026-10-27, against spec v0.345 (origin/main
`c7f491704`) and the r35 pre-release compiler `bezelc 0.0.0-f6863e62`. Every program below is
under `relaxations/`, was compiled in both disposal modes with `relaxations/run_all.sh`, and
its actual verdict and output are in `relaxations/results.md`. Nothing in `docs/`, `tests/`
or `src/` was touched.*

## 1. The short answer

The mutation table in `bzcore/README.md` counted a program as "sound" when every run either
finished with each object disposed exactly once **or stopped with the deterministic checked-mode
fault** (`DisposedAccessException`). That is the spec's own definition of the K4.1 backstop,
but it blurs the distinction the owner is asking about, so this note uses three grades:

- **(a) statically equally sound** — no program that the relaxed rule admits can fail at run
  time in any way the current rule prevents;
- **(b) flag-backstopped** — the relaxed rule admits programs that *do* fail, but only as a
  deterministic `DisposedAccessException` in checked mode; in an unchecked build the same
  program is undefined behaviour (we observed a segfault and a silent wrong read). This is
  *not* "equally sound" in the static sense; it is the posture explicit `Dispose()` already
  has today;
- **(c) unsound** — a leak, a double disposal, a data race, or a read of freed memory that no
  flag catches.

With that vocabulary:

| rule | what the bzcore row actually did | grade of the change | verdict |
|---|---|---|---|
| **K5.3** (explicit `Dispose()` under a live borrow is *admitted*) | made it **stricter** (froze `Dispose()` too): 60 programs newly rejected, 0 newly admitted | the current rule is already the relaxed end; the stricter variant is pure over-rejection | **nothing to retire.** K5.3 is the relaxed choice, recorded as such in rationale §5.1 and DECLINED ("freezing explicit `Dispose()` too — a de-facto borrow checker for no soundness gain"). The only further step — unfreezing the two recorded exceptions K5.7 (`scoped` captures) and K5.8 (guard interiors) — is grade (c). |
| **K5.2** (drop-on-overwrite of a binding under a live borrow is *rejected*) | deleted it: 72 programs newly admitted, every one of them ends in the checked-mode fault | **(b)** — exactly the posture `Dispose()` has; the spec says so itself (K5.2 "fails (i)": it is a readability rule, "the flag it does set is irrelevant once (i) fails") | **relaxable, as a recorded option, not a recommendation.** The exact edit is one row flip plus a mandatory warning (§4.2). The cost is that a silent line (`a := new …`) becomes a kill site the reader is not warned about; the benefit is small (the rejected idiom has a one-line rewrite: dispose first, then reassign — which the compiler already admits, see k52_p3). Keep unless an ergonomic case appears. |
| **K3.13** (an unmarked read never consumes) | flipped it: unmarked reads *move*; 1,008 newly admitted, 1,638 newly rejected, 0 unsound | **(a)** inside the toy model — but it is a different language, not a relaxation of this one: in bzcore every read is a move because the model has no plain parameters, so "sound" there means "Rust without `&`" | **keep.** The decision record (rationale §5.10's three reasons, re-affirmed for every position at v0.246, rationale §3.10) and two recorded declines (DECLINED: the borrow checker; proposal 0137 §3.4's flipped `using var` default) all weigh the same trade: a marker on the rare operation (transfer, 391 `move` sites in the corpus and guides) versus a marker on the common one (borrow, every plain argument and `var b := a`). |

One compiler/spec finding fell out of the programs (§5): the compiler admits `a.Dispose();
a := new Widget(…)` under a live borrow of `a`'s old value (`k52_p3`, `k52_p5`), which by the
letter of K5.2 is a frozen drop-on-overwrite — the admission is the sensible reading (the kill
was announced one line earlier and the surviving borrow is flag-backstopped), but the text does
not say it, and it is worth a one-line clarification rather than a silent agreement.

## 2. K5.3 — explicit `Dispose()` while borrowed stays admitted

**The rule today (v0.345, §28.3.1 K5 clause 3).** *"the K4.2 site-announced kills (rows K3.5,
K3.8, K3.9) are unfrozen — each satisfies (i), and satisfies (ii) wherever the surviving borrows
are backstopped by the kill's own flag, which is every case but the two recorded in clauses 7
and 8"*. In plain words: you may call `x.Dispose()` even while another local still refers to
`x`'s object; the compiler does not stop you, and any later use through that other local raises
`DisposedAccessException` in checked mode (the default build).

**Programs.**

`k53_p1_dispose_under_live_borrow.bzl` — admitted today; the read through `b` faults:
```bezel
var a := new Widget("a");
var b := a;                 // unmarked read: a borrow of a (K3.13)
a.Dispose();                // site-announced kill (K3.9) under the live borrow b
Console.WriteLine(b.Name);  // use through the surviving borrow
```
`k53_p2_dispose_after_last_use.bzl` — the same shape with `b`'s last use *before* the kill:
```bezel
var a := new Widget("a");
var b := a;
b.Touch();
a.Dispose();
Console.WriteLine("done");
```
`k53_p3_dispose_then_rebind_borrow.bzl` — use of the disposed binding itself (§28.2's warning):
```bezel
var a := new Widget("a");
a.Dispose();
var c := a;                 // borrow of an explicitly-disposed binding: mandatory warning
Console.WriteLine(c.Name);  // faults in checked mode
```

| program | spec verdict | compiler (r35) | checked run | unchecked run | under the *stricter* mutation (K5.3 frozen) |
|---|---|---|---|---|---|
| k53_p1 | accept | accept | `dispose a`, then `DisposedAccessException`, exit 70 | `dispose a`, then **segfault** (exit 139) | newly **rejected** |
| k53_p2 | accept | accept | `touch a / dispose a / done`, exit 0 | same | unchanged (b is not live at the kill) |
| k53_p3 | accept-with-warning (§28.2) | accept + `warning: use of a after a dominating explicit Dispose() call` | fault, exit 70 | segfault | unchanged (no borrow is live *at* the kill; the use comes after) |

Spec and compiler agree on all three.

**Soundness verdict.** The bzcore row *flipped* K5.3 to the stricter side and found only
over-rejection (60 of 12,327 accepted programs lost, none of them unsound). That means K5.3 is
**already the relaxed choice**: it is the place where the language deliberately stops short of a
borrow checker and leans on the flag. There is no further relaxation of K5.3 itself — a kill
cannot be "more admitted" than admitted. The two places where `Dispose()` *is* frozen (K5.7,
sources captured under a `scoped` storage admission; K5.8, a `§30.12` guard's interior borrows)
are the two shapes where the flag is not an admissible backstop — the stored callback's later
invocation is invisible at the kill, the guard's interior carries no flag at all — and
unfreezing either is grade **(c)**.

What k53_p1 *does* show is the price of the current posture: in an unchecked build the program
segfaults. That is the recorded trade (rationale §5.1: "use-after-dispose is not statically
prevented; it's caught at runtime"), and checked disposal being the default build (v0.307,
rationale §5.30) is what makes it acceptable.

**Recommendation: keep; nothing to retire.** One optional *lint* is worth recording, not
ruling: a non-mandatory warning at a `Dispose()` site when a live local borrow of the receiver
exists in the same body (the K5.5 liveness pass already computes it), so that k53_p1 is as loud
at compile time as k53_p3 is. Evidence needed before even that: a corpus count of how often the
shape occurs intentionally (dispose early, keep a borrow for a non-member use such as identity
comparison), which the trust-admission report mode (`--report-trust-admissions`) does not cover
today.

## 3. K5.2 — drop-on-overwrite of a binding while borrowed is rejected

**The rule today (v0.345, §28.3.1 K5 clause 2).** *"drop-on-overwrite of a binding (row K3.11)
is frozen although it disposes — it fails (i): the reassignment is not keyword-visible as a kill
of the old value, and the flag it does set is irrelevant once (i) fails"*; §28.7 *Overwrite while
borrowed*: *"Reassigning a plain-`var` owned local while a live local borrow of its current value
exists is rejected by the consume-site freeze (K5.2) — the implicit disposal would silently
invalidate the borrow."* In plain words: `a := new Widget(…)` disposes the old widget, and if
another local still refers to it the compiler rejects the assignment — not because the later use
would be uncaught (the flag *would* catch it) but because nothing on the line says "kill".

**Programs.**

`k52_p1_overwrite_under_live_borrow.bzl` — rejected today:
```bezel
var a := new Widget("first");
var b := a;                 // borrow of a's current value
a := new Widget("second");  // drop-on-overwrite disposes "first" silently (K3.11)
Console.WriteLine(b.Name);  // b survives the kill
```
`k52_p2_unborrowed_reassignment.bzl` — admitted today (K5.2 only bites under a live borrow):
```bezel
var a := new Widget("first");
a.Touch();
a := new Widget("second");
a.Touch();
```
`k52_p3_dispose_then_overwrite_under_borrow.bzl` — the keyword-visible spelling of p1:
```bezel
var a := new Widget("first");
var b := a;
a.Dispose();                // the kill is announced here (K5.3: admitted)
a := new Widget("second");  // reassigning an explicitly-disposed LHS: nothing displaced
Console.WriteLine(b.Name);  // faults in checked mode
```
`k52_p4_overwrite_in_loop_under_borrow.bzl` / `k52_p5_dispose_then_overwrite_in_loop.bzl` —
the same pair around a loop back edge (the borrow taken before the loop stays live).

| program | spec verdict | compiler (r35) | checked run | unchecked run | under the mutation (K5.2 deleted) |
|---|---|---|---|---|---|
| k52_p1 | **reject** (K5.2) | reject: `cannot consume a here: the borrow held by b is still live … (consume-site freeze)` | — | — | newly **admitted**; would behave exactly like k52_p3 |
| k52_p2 | accept (+ must-use warning, K3.14) | accept + must-use warning | `touch first / dispose first / touch second / dispose second` | same | unchanged |
| k52_p3 | by the letter: reject (K5.2, see §5); by the criterion: accept | **accept** + must-use warning | `dispose first / dispose second / DisposedAccessException`, exit 70 | `dispose first / second / dispose second`, exit 0 — **`b.Name` prints `second`**: the freed block was reused by the new widget, a silent wrong read | unchanged |
| k52_p4 | reject (K5.2) | reject (same diagnostic, at the loop body's store) | — | — | newly admitted; behaves like k52_p5 |
| k52_p5 | as k52_p3 | accept | `w0 / dispose w0 / dispose w1 / DisposedAccessException` | `w0 / dispose w0 / w1 / dispose w1 / dispose w2`, exit 0 — again the stale borrow reads the replacement | unchanged |

**Soundness verdict: (b), exactly.** Deleting K5.2 admits programs whose run-time behaviour
already exists in the language under a different spelling: k52_p1-relaxed *is* k52_p3. Every
program bzcore's mutation newly admitted (72 of 72 with a branch, 408 of 408 straight-line) ends
in the checked-mode fault, never in a leak, double disposal or race — because drop-on-overwrite
*disposes*, and disposal sets the flag. That is grade (b): a deterministic fault where today
there is a compile error, and undefined behaviour in an unchecked build — which k52_p3's
unchecked run makes concrete: no crash, just `b` quietly reading the *new* widget's name.

Where the model's "(b)" would stop being true in the full language, and must be checked before
any edit: the surviving borrow must be one the flag reaches. That holds for class and interface
borrows and, since v0.332 (proposal 0153), for span-carried borrows; it fails for the two
recorded K5 shapes (K5.7 `scoped` captures, K5.8 guard interiors), which stay frozen under
any variant; and it needs re-checking for a borrow held by a live *frame local* (K5.5 traces
an iterator frame as a borrow of its sources; the frame's own disposal counts as a use, and a
frame may read the overwritten binding's old object through a span it holds — flag-checked
again since 0153, so (b) should hold, but bzcore has no frames to confirm it).

**What K5.2 buys, in the spec's own words:** *"(i) the reader's notice"*. The one idiom it
rejects — borrow, reassign the source, keep using the borrow — has a mechanical rewrite that the
compiler accepts today (`a.Dispose(); a := …`), so its ergonomic cost is one extra line, and the
rewrite is more honest code. The rationale (§5.1) also records why the freeze was applied to
*every* consume site rather than to a transfer subset: positions are not closed under
composition and the rule had to compose with §28.7. Unfreezing overwrite alone re-opens that
seam by one row, which is small but not free.

**Recommendation: keep; record the relaxation as an option.** If the owner wants it, the
exact edit shape is:

1. K5 clause 2 (§28.3.1): *"drop-on-overwrite of a binding (row K3.11) is **unfrozen** under
   its flag, as the K4.2 kills are, and the overwrite under a live borrow draws a **mandatory
   warning** naming the surviving borrow"* — i.e. keep (ii), waive (i) with a warning in its place.
2. The consume table, row K3.11, Frozen column: `frozen (K5.2)` → `unfrozen (K5.2†)` with the
   same † the K4.2 rows carry (K5.7/K5.8 excepted).
3. §28.7 *Overwrite while borrowed*: rejected → warned, with the k52_p1 program as the example and
   its checked-mode fault stated.
4. Appendix C.3's freeze bullet, the code-generation reference §4.4 ("Frozen: every transfer … and
   reassignment") and the Guide 01 line that teaches the rewrite.
5. Flips: the pin map shows 2 disposal-matrix cells citing K5.2; in the test suite the
   rejection is pinned directly by `consume_site_freeze.reassign_while_borrowed_spec_28_7` and
   by the flipped-to-reject rows of `owned_local_reassign_913_1054`, and the freeze diagnostic
   needle (`the borrow held by … is still live`) appears in 24 gtest files and 2 lit files, some
   of which exercise the overwrite row; a behaviour-flip grep per CLAUDE.md would name each, and
   each K5.2 one becomes a `warn`/`run` case. No rule-corpus case quotes K5.2 verbatim (pin map,
   table A).

Evidence to gather first, beyond bzcore: (i) the full-language shapes listed above (frame
locals, spans over the overwritten binding, captured bindings — the latter cannot be reassigned
at all, K2+K5.5, so they are unaffected) checked on the compiler with ASan
(`--sanitize=address`, `--link=system`); (ii) a corpus/test count of programs the rule rejects
*today* that are not of the dispose-then-reassign shape — if it is near zero, the relaxation buys
nothing and should stay recorded, not landed; (iii) the process' usual critic pass, since this
is a design change under spec-change-process §2 (it contradicts a recorded decision, so it needs
new evidence against rationale §5.1's stated reason — and "the flag catches it" is the reason the
record already weighed and set aside in favour of the reader's notice).

## 4. K3.13 — an unmarked read never consumes

**The rule today (v0.345, §28.3.1 K3 clause 13).** *"A consume is spelled by the transfer
marker (`move x`) except at a marker-free position, where the target or keyword already announces
it … An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow
for class-typed `a`."* In plain words: reading a variable never takes ownership away from it; to
give ownership away you write `move`, except where the destination already says so (`return`
under `owned`, `throw`, an `owned` field store, …).

**Programs.**

`k313_p1_unmarked_read_is_borrow.bzl`:
```bezel
using var a := new Widget("a");
var b := a;        // a borrow; a stays owned and usable
a.Touch();
b.Touch();         // one object, two names, one disposal at scope end
```
`k313_p2_owned_argument_needs_move.bzl` / `k313_p3_owned_argument_with_move.bzl`:
```bezel
var s := new Sink();            // Sink.Adopt(owned w: Widget)
var w := new Widget("w");
s.Adopt(w);                     // p2: rejected — "this call consumes w; write M(move w)"
s.Adopt(move w);                // p3: admitted; w is moved-from afterwards
Console.WriteLine("after");
```
`k313_p4_plain_argument_is_borrow.bzl`:
```bezel
using var w := new Widget("w");  // Program.Log(w: Widget) — a plain parameter
Log(w);
Log(w);                          // a borrow each time; the caller keeps ownership
w.Touch();
```

| program | spec verdict | compiler (r35) | run | in the flipped language (unmarked read consumes) |
|---|---|---|---|---|
| k313_p1 | accept | accept | `touch a / touch a / dispose a` | `var b := a` **moves**; `a.Touch()` is a read of a moved binding → **rejected**; to keep today's meaning you would write a borrow marker (`var b := &a` or similar) |
| k313_p2 | reject (§22.3: the marker is required) | reject: `this call consumes w; write M(move w)` | — | **admitted**: the bare argument is the move |
| k313_p3 | accept (+ must-use warning on the undischarged `owned` parameter, K3.14) | accept + that warning | `touch w / dispose w / after` | the `move` becomes redundant (an error at a marker-free position, or admitted-but-noisy) |
| k313_p4 | accept | accept | `log w / log w / touch w / dispose w` | the first `Log(w)` **moves `w` into `Log`** (whose parameter is plain — in the flipped language a plain parameter *takes* unless the call borrows explicitly); the second `Log(w)` and `w.Touch()` are **rejected** |

Spec and compiler agree on all four.

**What the flipped language is.** It is the Rust/affine default: every read is a transfer, and
*borrowing* is the operation that needs a marker. bzcore found it "equally sound" (0 unsound
among 1,008 newly admitted programs) because, with reads consuming, far fewer borrows are ever
live, so the freeze rarely fires and every moved-from binding is statically unreadable (K1.3).
That result is real but says nothing about *this* language: bzcore has no plain parameters, no
fields, no `using var` pins, no closures, and its only "borrow" form is `var b := a`. In the
real language the flip would:

- make every plain-parameter call (`Log(w)`, `Console.WriteLine(w.Name)` is fine but `Use(w)` is
  not) a transfer unless marked — k313_p4 shows the common case breaking;
- need a new borrow marker (the thing Bezel deliberately does not have: rationale §5.1 "`owned`,
  not borrows"), or a last-use inference ("move if this is the binding's last read") — which is
  rationale §5.10's reason 2 against inferred take-out: *"Adding a `Log(...)` call between read
  and reassignment … would silently change whether the read was a borrow or a take"*;
- invert the reading model the code-generation reference calls *"the single most important
  spelling fact in the language"* (§4.2's no-silent-consume rule), re-teach Guide 01, and flip
  thousands of sites: the corpus and guides contain 391 `move` spellings against roughly 2,600
  call-argument lists and 214 bare `var x := y;` reads — the marker would move from the rare
  operation to the common one.

**The decision record.** Rationale §5.10 (v0.87) chose the explicit `move` for three reasons —
visibility in review, refactoring stability, the exception-path obligation — and the v0.246
unification (rationale §3.10; proposal 0090) took them "at full generality": a named-binding
consume is spelled at the site, everywhere. DECLINED.md records the adjacent declines: a
compile-time borrow checker (`rejected`, rationale §5.1), and proposal 0137 §3.4's *option D*
— flipping the default so that an unmarked `using var y := x;` adopts — `declined-at-proposal`
precisely because *"it amends K3.13 … and makes `using var` the one binding keyword under which
an unmarked read consumes"*. The language does keep one unmarked-move regime on purpose: move-only
*value* types (owning inline arrays, owning choices; K3.12, §15.3) move on unmarked assignment
because a copy is impossible, so there is nothing for a marker to disambiguate. That is the
principled boundary: the marker exists exactly where a read *could* mean two things.

**Soundness verdict: (a) in the model, but it is not a relaxation.** Flipping K3.13 admits
some programs (k313_p2) and rejects others (k313_p1, k313_p4); it is a different language with
the same soundness. There is no grade here because nothing is being weakened.

**Recommendation: keep.** One smaller option is worth recording, not pursuing: admitting the
*omission* of `move` at an `owned` argument when the binding has no later use on any path
("implicit move at last use"). It is grade (a) — the compiler already computes liveness for K5 —
but it is the inference rationale §5.10 reason 2 declines, and the compiler's current diagnostic
(`this call consumes w; write M(move w)`) is a one-token fix. If the owner wants to weigh it, the
evidence is a count of `move`-at-argument sites in the corpus that are last uses (most, probably)
and a review of how many would become ambiguous after an unrelated edit.

## 5. Findings about today's compiler (not papered over)

1. **Reassignment of an explicitly-disposed binding under a live borrow is admitted** (k52_p3,
   k52_p5). By the letter, K5 freezes "consuming an owned binding … while a live local borrow of
   its current value exists" and K5.2 freezes "drop-on-overwrite of a binding"; §28.3.3's
   state-match rule 3 says an explicitly-disposed LHS "displaces a logically disposed value: the
   store is that value's release point". Nothing says the freeze is lifted when the displaced
   value is already disposed. The compiler's reading is the sensible one — the kill was announced
   by the `Dispose()` and the surviving borrow is flag-backstopped, so K5's conjuncts (i) and (ii)
   both hold *for the earlier kill* — but the spec should say it in one sentence (K5.2 or §28.7
   *Overwrite while borrowed*: "a reassignment that displaces an explicitly-disposed or moved-from
   value kills nothing and is not frozen"). Filed as a spec question in this report, not on GitHub
   (lane rule).
2. **Checked mode works as specified** for every program here: each stale read through a borrow
   raised `DisposedAccessException` (exit 70), including through the loop back edge (k52_p5) and
   after an overwrite-at-release-point store (k52_p3).
3. **Unchecked mode shows both faces of UB**: a segfault (k53_p1, k53_p3) and a silent wrong read
   (k52_p3, k52_p5: `b.Name` prints the *replacement*'s name because the store reclaimed the
   disposed block and the new object landed in it). The second face is the more dangerous one and
   is a good example for Guide 01's "never depend on unchecked mode" line.
4. Minor: the K5.2 diagnostic says the later use *"would read freed memory"*; in the default
   checked build it would raise `DisposedAccessException`. Wording only.
5. Minor: the must-use warning (K3.14) is emitted beside the freeze error in k52_p1/p4; harmless,
   but a rejected program's secondary lint is noise.

## 6. Files

- `relaxations/*.bzl` — the twelve programs (`common_widget.txt` is the shared class).
- `relaxations/run_all.sh` — compiles each in `--checked-disposal` and `--unchecked-disposal`
  with the r35 pre-release and writes `relaxations/results.md` (verdicts, diagnostics, output,
  exit codes); `relaxations/out/` holds the binaries and raw logs.
