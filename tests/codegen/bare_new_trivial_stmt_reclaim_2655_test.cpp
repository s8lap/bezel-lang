// Codegen — issue #2655, shape 1 (§21.9 statement form of expressions, §28.3
// unbound owned temporaries, §28.9 trivial-dispose classes; spec v0.237): a
// BARE owned-producer statement — `new Box(7);` on a line of its own — emitted
// no free anywhere and leaked, silently.
//
// The governing spec text is §28.3's *Unbound owned temporaries*: "an unnamed
// owned temporary — which no named form can reach — is reclaimed automatically
// once the expression that produced it is finished with it", with the boundary
// fixed by the same subsection: "in an expression statement, 'end of the full
// expression' and 'end of the statement' coincide ([§21.9])". §28.9 in turn
// says trivial-dispose instances' memory "is reclaimed by an
// implementation-defined mechanism ... that runs no user code" — the
// compiler-emitted end-of-statement free this fix adds is a choice of exactly
// that mechanism.
//
// Root cause (read on main ce1c6a8 before the fix): the ExpressionStmt arm of
// `lowerStmt` (src/codegen/LlvmBackend_Stmts.cpp:2289-2292) was
// `if (es->expr) lowerExpr(*es->expr);  // side effects only` — the produced
// pointer was dropped on the floor with no disposal registered on either edge.
// Nothing upstream compensated: §21.9's walk (src/sema/ExprTypecheck.cpp
// ~9612-9680) hard-ERRORS only the NON-trivial-dispose case, so a trivial `new`
// sailed through with no diagnostic at all, and the ownership pass's
// end-of-statement reclaim flag existed only on DiscardStmt (#1009's
// `disposeOwnedValue`, Ownership.cpp ~2508), never on ExpressionStmt. The
// asymmetry was the tell: `_ := new Box(7);` — the SAME statement plus §17.13's
// cosmetic discard sink — has been freed since #1009, while the bare spelling
// leaked.
//
// Fix: `ast::ExpressionStmt::disposeOwnedValue`, set by SEMA inside §21.9's
// OWN WALK (src/sema/ExprTypecheck.cpp -- the single setter), NOT by the
// ownership pass and NOT under `producerState == Owned`, consumed by a codegen arm
// that is a verbatim copy of the DiscardStmt::disposeOwnedValue arm (one-entry
// dispose scope -> both edges; `dynamicDispose` dispatches Dispose+reclaim
// through the object's own vtable, null-guarded). The two spellings now agree.
//
// SCOPE (why the freed set is safe): the flag is derived from §21.9's own
// trivial-dispose verdict, so the freed set is a STRICT SUBSET of what §21.9
// inspected and declined to reject -- the two cannot disagree by construction,
// and they widen together when #2680 lands. An earlier draft keyed on the
// ownership pass's `producerState == Owned`, which is far wider than §21.9's
// rejection and therefore ran USER DESTRUCTOR CODE at end of statement
// (demonstrated, and pinned by
// non_trivial_producer_outside_spec_21_9_keeps_its_destructor_unrun below).
// Do not re-derive the gate from the paragraph that follows.
//
// SOLE-OWNER PROOF (why this cannot double-free): `producerState == Owned`
// admits ONLY a fresh production — a `new`, an `owned`-returning call, a `move`
// result (§28.3's *Producers*; K1.5 "Fresh productions are owned"). Every
// BORROW is classified Borrowed and left unmarked: a field or local read, and
// in particular §21.9's own fluent-receiver shape `sb.Append(x);` (a plain,
// non-`owned` instance-method return, which K1.5 makes "a borrow whose source
// is the receiver"). So the only value this arm ever frees is one no named
// binding, no slot, and no other holder has — there is no second free site to
// race. The `fluent_receiver_bare_statement_not_freed` case below pins that
// mechanically.
//
// EXACTLY-ONCE ORACLE. A trivial-dispose class has, by definition (§28.9), no
// destructor — so a *user-visible* live-object counter is impossible to write
// for these shapes: there is no user code to count from. The sanitizer is the
// counter: under `runAotChecked` a MISSING free is an LSan leak report and a
// SECOND free is an ASan double-free/UAF abort, and both flip `r.ran` to false
// -> the -999 sentinel. Pairing that with the program's computed value in one
// EXPECT_EQ pins under-count, over-count, and use-of-freed-data together.
//
// fail-without/pass-with (measured on ce1c6a8 with `--sanitize=address`):
// `new Box(7);` leaked 24 B in 1 allocation; the 8-iteration loop case leaked
// 192 B in 8 allocations. With the fix every case below is LSan/ASan-clean.
// per §21.9, §28.3, §28.9 (issue #2655, spec v0.237)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// -999 on any sanitizer fire (leak / double-free / UAF), else the real exit
// code — one EXPECT_EQ pins both memory-cleanliness and the computed value.
// Mirrors set_literal_backing_reclaim_2142_test.cpp's `runChecked`.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// Value-only (no ASan instrumentation) for the cases whose point is that a
// value stays READABLE — #2629's lesson is that ASan can mask a wild free that
// the plain build crashes on, so the no-double-free controls below assert on
// the plain path too.
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's exact repro. A bare `new Box(7);` statement.
// Without the fix: 24 B leaked in 1 allocation (measured on ce1c6a8).
// Value: 0. // per §21.9, §28.3, §28.9 (issue #2655)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, bare_new_statement_spec_21_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    new Box(7); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── LOUD form: the same statement in a loop, so a missing free is 8 leaked
// allocations rather than 1 (a single-shot leak is the shape #2143's original
// report got wrong as an LSan false-negative). Without the fix: 192 B in 8
// allocations. The free must fire per ITERATION, not once — the disposal is
// registered in the loop body's own one-entry scope. Value: 8.
// per §21.9, §28.3 (issue #2655)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, bare_new_in_loop_frees_per_iteration) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..8 do "   // §17.9: `a..b` is INCLUSIVE — 8 iterations
        "      new Box(i); "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        8);
}

// ── The OTHER owned-producer shape §21.9 admits for a trivial-dispose type: a
// bare `owned`-returning static call. §28.3 makes no distinction between
// producer kinds ("This rule is uniform across the source of the temporary"),
// and neither does the gate — producerState is Owned for both. Value: 0.
// per §28.3, §21.9 (issue #2655)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, bare_owned_returning_call_statement_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method MakeBox(): owned Box "
        "    return new Box(5); "
        "  end MakeBox; "
        "  public static method Main(): int "
        "    C.MakeBox(); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── DOUBLE-FREE PRE-EMPT (the one that matters). §21.9's *fluent-receiver
// discard* shape: `sb.Append(x);` as a bare statement. `Append` returns the
// receiver — a BORROW, per K1.5's "an instance method's plain (non-`owned`)
// class-typed return is a borrow whose source is the receiver" — and the
// receiver's `using var` already owns it. If the new arm's gate leaked past
// `producerState == Owned` and fired here, the builder would be freed at the
// statement AND again at scope end: a double free (ASan) reading a freed
// builder in between (UAF). Asserted BOTH ways: ASan/LSan-clean under
// runChecked, and the value still correct on the plain, uninstrumented path
// (#2629 — ASan can render a wild free benign that the plain build crashes on).
// Value: "abcd".Length = 4. // per §21.9, §28.3 (issue #2655 self-pass)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, fluent_receiver_bare_statement_not_freed) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"ab\"); "
        "    sb.Append(\"cd\"); "
        "    return sb.ToString().Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 4);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 4);
}

// ── UAF PRE-EMPT: a bare statement whose value is a BORROW of a live object
// reachable afterwards. `h.Peek();` returns a borrow of the holder's own field
// (receiver-sourced, K1.5) — freeing it would destroy `h`'s field and the
// following read would be a use-after-free. Must stay untouched: the object is
// read after the bare statement and must still hold its value.
// Value: 3 + 1 = 4. // per §28.3, §21.9 (issue #2655 self-pass)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, borrow_returning_bare_statement_not_freed) {
    const std::string prog =
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class Holder "
        "  public owned inner: Box := new Box(3); "
        "  public method Peek(): Box "
        "    return this.inner; "
        "  end Peek; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Peek(); "
        "    return h.inner.V + 1; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 4);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 4);
}

// ── CONSTRUCTOR-SIDE-EFFECT axis: the free must run AFTER the constructor has
// fully executed, and the side effect must survive it. A trivial-dispose class
// whose constructor mutates an enclosing-scope counter through a borrowed
// parameter — the value written must be readable after the instance is freed.
// Value: 5. // per §28.3, §28.9 (issue #2655 self-pass)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, constructor_side_effect_survives_the_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Counter "
        "  public N: int := 0; "
        "end Counter; "
        "class Box(c: Counter) "
        "  public method Bump(): () "
        "    c.N := c.N + 5; "
        "  end Bump; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var k := new Counter(); "
        "    var b := new Box(k); "
        "    b.Bump(); "
        "    new Box(k); "
        "    return k.N; "
        "  end Main; "
        "end C;"),
        5);
}

// ── THE GATE'S OWN BOUNDARY (reviewer-directed). This is the case that forced
// the flag's setter out of the ownership pass and into §21.9's walk.
//
// `move r;` in statement position is an owned production — the ownership pass
// classifies it `producerState == Owned` — but it is NOT one of §21.9's
// `ownedProduced` shapes (not a `NewExpr`, not a `CallExpr`), so §21.9's hard
// error does not fire on it even though `Res` is NON-trivial-dispose. Under the
// first draft's `producerState == Owned` gate this arm would therefore have
// fired here and `dynamicDispose` would have run `Res`'s USER DESTRUCTOR at end
// of statement — contradicting the §28.9 sentence the whole fix rests on
// ("reclaimed by an implementation-defined mechanism … that runs no user code")
// and proposal 0077 §2.2, which scopes silent reclaim to trivial-dispose alone.
//
// Here the exactly-once oracle IS writable, and it inverts: a non-trivial class
// HAS a destructor, so a live-object counter can prove the free did NOT happen.
// `Res.alive == 1` after the statement means the destructor never ran. (Under
// the rejected wider gate this program returned 0.)
//
// runAot, not runAotChecked, and deliberately: `move r;` leaves `r` moved-from,
// so nothing owns the value and it LEAKS 16 B — verified. That leak is the
// correct, safe direction for a shape neither §21.9 nor this fix claims, and it
// is exactly what the separately-filed §21.9 under-rejection covers. Asserting
// it under LSan would fail on the expected leak and hide the actual point.
// per §21.9, §28.9, §28.8.1 (issue #2655, reviewer-directed self-pass)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655,
           non_trivial_producer_outside_spec_21_9_keeps_its_destructor_unrun) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class Res "
        "  public static alive: int := 0; "
        "  public constructor() Res.alive := Res.alive + 1; end constructor; "
        "  destructor Res.alive := Res.alive - 1; end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var r := new Res(); "
        "    move r; "
        "    return Res.alive; "   // 1 == the destructor did NOT run
        "  end Main; "
        "end C;"),
        1);
}

// ── SCOPE RE-HOMING PRE-EMPT (reviewer-directed). `new Widget(out owned var x);`
// as a bare statement: expression lowering itself pushes a disposal for the
// inline `out owned var` recipient `x` (ensureCtorOutVarSlots,
// LlvmBackend_Calls2.cpp) onto `disposeStack_.back()`. Had this arm opened its
// one-entry dispose scope BEFORE calling lowerExpr, that registration would have
// been re-homed from the enclosing block scope into the STATEMENT scope — so `x`
// would be disposed and its slot nulled at end of statement while still live for
// the rest of the block, and the `x.Id` read below would fault or read garbage.
// Lowering first, then opening the scope, keeps the registration where it
// belongs. Value: x.Id (9) + Res.alive (1) = 10 — proving `x` is intact AFTER
// the bare statement freed the Widget.
//
// HONEST LABEL — this is a CONTROL, not a discriminator: it passes with the
// scope opened before lowering too. Measured, not assumed: the untouched
// `_ := new Widget(out owned var x);` discard spelling, whose #1009 arm DOES
// open its scope first, returns the same 10 with `x` equally intact. So the
// re-homing hazard is real in principle but does not materialize on this shape;
// the lower-first ordering is kept because it removes the hazard structurally
// rather than relying on that measurement, and this test locks the observable
// in so a future reordering cannot regress it silently.
//
// runAot: `x` is not disposed at its own scope end either (16 B, `Res.alive`
// still 1 at the read) — a PRE-EXISTING `out owned var` gap, identical in that
// same untouched discard spelling, filed separately. This test pins the
// recipient's liveness, not that leak.
// per §22.3, §22.4, §28.3 (issue #2655, reviewer-directed self-pass)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655,
           out_owned_var_recipient_stays_live_after_the_statement_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class Res "
        "  public static alive: int := 0; "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; "
        "    Res.alive := Res.alive + 1; end constructor; "
        "  destructor Res.alive := Res.alive - 1; end destructor; "
        "end Res; "
        "class Widget "
        "  public constructor(out owned n: Res) "
        "    n := new Res(9); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    new Widget(out owned var x); "
        "    var v := x.Id; "
        "    return v + Res.alive; "
        "  end Main; "
        "end C;"),
        10);
}

// ── NESTED-BODY axis: the bare form inside a `do … end` block and inside the
// body of an `if`, so the free is registered in the INNER scope and fires at
// that block's end (not the method's). Without the fix both leaked.
// Value: 2. // per §21.6, §28.3 (issue #2655)
BEZEL_TEST(bare_new_trivial_stmt_reclaim_2655, bare_new_in_nested_blocks_spec_21_6) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    do "
        "      new Box(1); "
        "      n := n + 1; "
        "    end; "
        "    if n > 0 then "
        "      new Box(2); "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        2);
}
