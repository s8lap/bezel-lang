// fix #1987 — §17.8 (the null-forgiving operator `!`), §28.3 (producers), and
// §28.10 (`using var`), spec v0.226. Codegen-observable, LeakSanitizer-checked
// coverage for: a postfix null-forgiving `!` on a FRESH PRODUCER bound to an
// (untyped) `using var` — `using var c := producer()!` / `using var c := new
// Cell()!` — leaked the producer's 16B class block (destructor never ran, LSan
// flagged the leak). The `!`-free `using var c := producer()` was already
// clean; the typed `using var c: Cell := producer()!` was clean too (handled
// via the declared-type class path). Only the INFERRED `using var` over a
// PostfixBangExpr-wrapped producer leaked.
//
// Spec resolution. §17.8 (v0.226): "The null-forgiving operator preserves
// ownership: if `x` has ownership state *owned ... `x!` is owned; if `x` is
// borrowed, `x!` is borrowed with the same source. The operator is purely a
// type-system assertion (with a runtime null check); it does not move, copy,
// or borrow beyond what reading `x` already does." A fresh producer
// (`new T(...)`, a call declared `owned T`) is OWNED per §28.3's producers
// table, so `producer()!` is OWNED — the `!` on a temporary (not a place)
// carries the +1 ownership through (the field-store sibling is pinned by the
// #1983 producer_bang test). §28.10 binds that owned value to a `using var`
// with declared scope-end disposal, on every exit edge. The ownership pass
// already classified such a binding Owned (the using-var adoption), so the
// gap was purely codegen: the PostfixBangExpr wrapper was invisible to the
// CallExpr/NewExpr producer-recognition branches of
// usingVarOwnsClassObjectDynamically, so no disposal was registered and the
// producer leaked.
//
// The fix (src/codegen/LlvmBackend_Setup.cpp, branch (b0e) of
// usingVarOwnsClassObjectDynamically): peel the PostfixBangExpr and recognize
// the inner producer via the SAME shared discriminant the sibling leak fixes
// (#1934 case-expr result, #2204 borrowing-collection temp) and the (b0c) `??`
// RHS consult — isFreshOwnedClassProducer. A PLACE `E!` (a bare-id / field
// read) is a §17.8 borrow (and a `using var` over a tracked borrow is a
// §28.10 borrow-pin the ownership pass flags usingVarSourceIsBorrow, which
// makes registerUsingDisposal early-return before this classifier runs), so
// the peel is gated on isFreshOwnedClassProducer and never enables a
// double-free of a place.
//
// Every observable rides on a single `runAotChecked` run: the exit code
// encodes a destructor COUNT (a static counter bumped on dispose), and the
// same AOT+ASan/LSan run flips `ran` to false on any leak / double-free /
// use-after-free. FAIL-WITHOUT/PASS-WITH: before the fix the producer's
// destructor never ran (Cell.live stayed 1) and the block leaked (LSan flag);
// after the fix it is disposed exactly once with no leak.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class Cell whose destructor decrements a static live counter.
// A correct program leaves live==0; a leak leaves it >0; a double-free drives
// it negative or aborts under ASan (heap-use-after-free). The exact-once
// oracle.
const char* kCell =
    "class Cell "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

std::string prog(const std::string& classesAfterCell,
                 const std::string& mainBody) {
    return std::string("module M; ") + kCell + classesAfterCell +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE BUG (issue's own repro shape) — `using var c := Mk()!` where Mk is
// declared `owned Cell?`: the postfix `!` unwraps the fresh producer's
// nullable result and the using-var adopts the +1. The block is disposed
// exactly once at scope end. FAIL-WITHOUT: Cell.live == 1 (+ 16B LSan leak).
// ===========================================================================
BEZEL_TEST(using_var_producer_bang_leak_1987,
           owned_call_bang_using_var_disposes_once_17_8_28_10) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(): owned Cell? return new Cell(); "
        "  end Make; "
        "end Mk; ",
        "  do "
        "    using var c := Mk.Make()!; "
        "  end; "
        "  return Cell.live; ");  // 0 — disposed at the do-block's scope end
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// THE BUG (`new T()!` form) — `using var c := new Cell()!`: the `!` on a
// fresh `new` temporary carries the +1 ownership through (§17.8). Same
// leak shape as the call form; same fix path (isFreshOwnedClassProducer
// recognizes a NewExpr operand). FAIL-WITHOUT: Cell.live == 1 (+ leak).
// ===========================================================================
BEZEL_TEST(using_var_producer_bang_leak_1987,
           new_expr_bang_using_var_disposes_once_17_8_28_10) {
    const std::string src = prog(
        "",
        "  do "
        "    using var c := new Cell()!; "
        "  end; "
        "  return Cell.live; ");  // 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// Two `producer()!` using-vars in succession — each disposed exactly once,
// in reverse-declaration order. FAIL-WITHOUT: Cell.live == 2 (+ leak).
BEZEL_TEST(using_var_producer_bang_leak_1987,
           two_owned_call_bang_using_vars_dispose_once_each_28_10) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(): owned Cell? return new Cell(); "
        "  end Make; "
        "end Mk; ",
        "  do "
        "    using var a := Mk.Make()!; "
        "    using var b := Mk.Make()!; "
        "  end; "
        "  return Cell.live; ");  // 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// CONTROL — the `!`-free sibling `using var c := Mk.Make()` was already clean
// (the ordinary (b1) owned-call path). Guards that the (b0e) peel does not
// disturb it. A non-nullable `owned Cell` return is used so the `!`-free form
// typechecks without a nullability narrowing.
// ===========================================================================
BEZEL_TEST(using_var_producer_bang_leak_1987,
           owned_call_no_bang_using_var_still_disposes_once_28_10) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(): owned Cell return new Cell(); "
        "  end Make; "
        "end Mk; ",
        "  do "
        "    using var c := Mk.Make(); "
        "  end; "
        "  return Cell.live; ");  // 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// CONTROL — the TYPED sibling `using var c: Cell := Mk.Make()!` was already
// clean (classOfLocal recovers the class from the declared type, so
// registerUsingDisposal's static class-of-local path fires before
// usingVarOwnsClassObjectDynamically is reached). Guards that the (b0e) peel
// is reached only for the INFERRED form.
// ===========================================================================
BEZEL_TEST(using_var_producer_bang_leak_1987,
           typed_owned_call_bang_using_var_still_disposes_once_28_10) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(): owned Cell? return new Cell(); "
        "  end Make; "
        "end Mk; ",
        "  do "
        "    using var c: Cell := Mk.Make()!; "
        "  end; "
        "  return Cell.live; ");  // 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// CONTROL — a PLACE `E!` (a force-unwrap of a NAMED owned local) into a
// `using var` does NOT take a second disposal: the named local still owns
// the value and auto-drops it at its own scope end (the `!` is a borrow per
// §17.8 — "if `x` is borrowed, `x!` is borrowed with the same source"; the
// (b0e) peel is gated on isFreshOwnedClassProducer, so a bare-id operand
// never matches). Guards against a double-free. Encoding: Cell.live after
// both scopes end.
// ===========================================================================
BEZEL_TEST(using_var_producer_bang_leak_1987,
           place_bang_using_var_does_not_double_free_17_8) {
    const std::string src = prog(
        "",
        "  do "
        "    var x: Cell? := new Cell(); "
        "    do "
        "      var xv := x!; "        // borrowing alias of the owned `x` (spec v0.350 §28.10: `using var c := x!` is rejected)
        "      using var c := xv; "   // borrow; x still owns
        "    end; "                   // c disposes nothing here
        "  end; "                     // x auto-drops here -> Cell.live == 0
        "  return Cell.live; ");      // 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// EXCEPTION-PATH AXIS — a `using var c := Mk.Make()!` disposed on the
// exception-unwind edge too (§28.10: disposal on every exit edge). A throw
// after the using-var's establishment runs the scope-end disposal during
// unwind; the catch observes Cell.live == 0 (disposed exactly once) and no
// leak / double-free. Encoding: caught*10 + Cell.live.
// ===========================================================================
BEZEL_TEST(using_var_producer_bang_leak_1987,
           owned_call_bang_using_var_disposed_on_unwind_28_10_28_2) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(): owned Cell? return new Cell(); "
        "  end Make; "
        "end Mk; "
        "class W "
        "  public static method Step(x: int) "
        "    if x > 0 then throw new InvalidOperationException(\"boom\"); end; "
        "  end Step; "
        "end W; "
        "class Run1 "
        "  public static method Go(): int "
        "    using var c := Mk.Make()!; "  // established here
        "    W.Step(1); "                  // throws — c disposed on unwind
        "    return 0; "
        "  end Go; "
        "end Run1; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run1.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + Cell.live; ");  // 1*10 + 0 = 10
    BEZEL_EXPECT_EQ(run(src), 10);
}
