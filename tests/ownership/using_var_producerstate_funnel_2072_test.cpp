// #2072 / §28.3 (v0.180) / §28.10 — conformance sweep: a `var x := E` and a
// `using var x := E` over the SAME right-hand side must produce the SAME K1
// ownership classification (Owned vs Borrowed), differing only in the `using
// var` pin/discharge layer. This is the invariant the #2072 refactor makes
// structural: `handleLocalDecl` computes EVERY initialized local's state through
// the ONE `producerState` funnel (`lo.state = producerState(*ld.initializer)`),
// and `ld.isUsing` is layered afterwards (scope-end discharge for an owned
// producer, a §28.10 borrow-pin over a borrow). Before the refactor a `using
// var` ran a PARALLEL classifier that defaulted to Owned + adoption and
// re-derived each borrow shape one special case at a time — the drift locus
// where #2071 (an unmarked owned bare-id classified adopt-and-move in the
// using-var path only) accumulated.
//
// The observable proxy for "same K1 classification" is exactly-once disposal
// with no double-free / leak: over an OWNED producer both binding forms dispose
// the value once (the `var` via §28.3 affine auto-drop, the `using var` via
// scope-end discharge); over a BORROW both leave the source's real owner to
// dispose it once (neither over-disposes). The `R.live` static counter
// (ctor +1 / dtor -1) is the exact oracle: net 0 ⇒ disposed exactly once; +N ⇒
// leak; −N / crash ⇒ double-free. runAotChecked additionally fails on any
// ASan/LSan report the counter alone would miss. Each pair pins that the two
// binding forms agree on the same RHS. // per §28.3, §28.10, #2072
//
// Spec: §28.3 "Ownership state of locals" (the producers algebra shared by
// `var` and `using var`), §28.10 (using-var scope-end discharge and the
// borrow-pin subsection). Derived from the #2072 acceptance conformance sweep.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A disposable class whose static `live` counter is the exact dispose-once
// oracle, plus a static owned-returning factory (a sourceless static factory is
// an OWNED producer per §28.3 / #1499).
const char* kR =
    "module M; "
    "class R "
    "  public static live: int := 0; "
    "  public constructor() R.live := R.live + 1; end constructor; "
    "  public method Ping(): int return 1; end Ping; "
    "  destructor R.live := R.live - 1; end destructor; "
    "end R; "
    "class Fac "
    "  public static method Make(): owned R return new R(); end Make; "
    "end Fac; ";

// Assert a program compiles, runs, and exits 0 (R.live == 0 at exit ⇒ exactly-
// once disposal), ASan/LSan-clean.
void expectDisposedOnce(const std::string& body) {
    JitResult r = runAotChecked(std::string(kR) + body);
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // surfaces a compile/link reject as a failure
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0); // R.live == 0 ⇒ disposed exactly once
}

}  // namespace

// ===========================================================================
// Pair 1 — a FRESH `new R()` is an OWNED producer under BOTH binding forms.
// `var` auto-drops it once at scope end (§28.3 affine auto-drop); `using var`
// discharges it once at scope end (§28.10). Net 0 either way. // per §28.3
// ===========================================================================
BEZEL_TEST(using_var_producerstate_funnel_2072, var_new_owned_disposes_once_spec_28_3) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do var x := new R(); var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — auto-dropped once
        "end Main; end P;");
}
BEZEL_TEST(using_var_producerstate_funnel_2072, using_var_new_owned_disposes_once_spec_28_10) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do using var x := new R(); var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — discharged once
        "end Main; end P;");
}

// ===========================================================================
// Pair 2 — a bare-id read of a named OWNED local is a BORROW under `var`
// (`var x := src` borrows, §28.3 local-read row): the SOURCE `src` retains the
// sole obligation and disposes exactly once. The `using var` twin over an
// owned local is a COMPILE ERROR since spec v0.350 (§28.10 *Adopting a named
// owned binding*), so its row reads a BORROW-STATE source instead: a plain
// parameter `b` bound to the caller's owned `src`. The pin disposes nothing,
// `src` disposes once (no double-free). This is the #2071 shape.
// // per §28.3, §28.10, v0.350, #2071
// ===========================================================================
BEZEL_TEST(using_var_producerstate_funnel_2072, var_bareid_borrow_source_disposes_once_spec_28_3) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do using var src := new R(); var x := src; var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — src disposes once, x borrows
        "end Main; end P;");
}
BEZEL_TEST(using_var_producerstate_funnel_2072, using_var_bareid_borrow_source_disposes_once_spec_28_10) {
    expectDisposedOnce(
        "class P "
        "  public static method View(b: R) using var x := b; var p := x.Ping(); end View; "
        "  public static method Main(): int "
        "    do using var src := new R(); P.View(src); end; "
        "    return R.live; "                     // 0 — src disposes once, x pins the borrow
        "  end Main; "
        "end P;");
}

// ===========================================================================
// ===========================================================================
// Pair 3 — an OWNED-returning static factory CALL is an OWNED producer under
// BOTH binding forms (§28.3 "Factory methods and sourceless returns" / #1499).
// `var` auto-drops the fresh value once; `using var` discharges it once. Net 0.
// // per §28.3
// ===========================================================================
BEZEL_TEST(using_var_producerstate_funnel_2072, var_owned_call_disposes_once_spec_28_3) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do var x := Fac.Make(); var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — auto-dropped once
        "end Main; end P;");
}
BEZEL_TEST(using_var_producerstate_funnel_2072, using_var_owned_call_disposes_once_spec_28_10) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do using var x := Fac.Make(); var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — discharged once
        "end Main; end P;");
}

// ===========================================================================
// Pair 4 — a bare-id read of an OWNED PARAMETER is a BORROW under `var` (the
// parameter retains the obligation and is disposed once by the frame). The
// unmarked `using var x := p;` is a COMPILE ERROR since spec v0.350 (§28.10
// *Adopting a named owned binding*; it named neither adopt nor view), so the
// row binds the view with `var`, the remedy the rule names.
// // per §28.10 v0.350, #2071
// ===========================================================================
BEZEL_TEST(using_var_producerstate_funnel_2072, using_var_owned_param_bareid_borrow_pin_spec_28_10) {
    expectDisposedOnce(
        "class P "
        "  public static method Take(owned p: R): int "
        "    var x := p; var n := x.Ping(); return 0; "  // view; p disposes
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new R(); var r := P.Take(move w); "
        "    return R.live; "                     // 0 — disposed once by the frame
        "  end Main; "
        "end P;");
}

// ===========================================================================
// ===========================================================================
// Pair 8 (F1 graft pin, landed with #2072) — a binding ATOM (§17.15 v0.216,
// #2098) whose BODY is a borrow classifies as a BORROW under both binding
// forms: producerState peels the atom to its body (§28.3 producers row), so
// `var` takes a plain borrow and `using var` takes a §28.10 borrow-pin — the
// pinned source's real owner disposes it exactly once. This is the branch the
// #2072 funnel landing had to GRAFT from the batch-B pre-funnel fork; without
// it the using-var form adopts and double-frees (the #2098 K8.5 hazard the
// batch-C review flagged as untested). // per §17.15/§28.10 v0.216
// ===========================================================================
BEZEL_TEST(using_var_producerstate_funnel_2072, var_bind_atom_borrow_body_disposes_once_spec_28_10) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do using var src := new R(); var x := (var q := 1; src); var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — src disposes once, x borrows through the atom
        "end Main; end P;");
}
BEZEL_TEST(using_var_producerstate_funnel_2072, using_var_bind_atom_borrow_body_pins_spec_28_10) {
    expectDisposedOnce(
        "class P public static method Main(): int "
        "  do using var src := new R(); using var x := (var q := 1; src); var p := x.Ping(); end; "
        "  return R.live; "                       // 0 — src disposes once, x PINS (no adoption)
        "end Main; end P;");
}
