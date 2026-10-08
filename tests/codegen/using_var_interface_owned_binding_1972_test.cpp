// fix #1972 / §28.6 — the UNMARKED analog of #1962's interface-source adoption
// fix. The normal-path disposal classifier `usingVarOwnsClassObjectDynamically`
// (src/codegen/LlvmBackend_Setup.cpp) registers a single dynamic (vtable-slot-2)
// dispose for a `using var y := …;` whose initializer is a bare identifier
// naming an interface-typed owned binding (`y := x;` where x is `owned IThing`)
// and `y` is UNANNOTATED. The MARKED analog (a1 branch, #1962, `var y := owned x;`)
// and the ANNOTATED analog (a branch, `var y: IThing := x;`) are already handled;
// the unmarked bare-id shape was missed, so the adopted value LEAKED at scope end.
//
// This is the unmarked sibling of #1962: §28.10's binding-position transfer
// (`using var y := owned x;`, marked) and the declaration-type pin
// (`using var y: IThing := x;`, annotated) both register dynamic disposal today,
// but the unmarked `using var y := x;` over an interface-typed owned source did
// not — a pre-existing gap (the bug predates #1962; #1962 newly exposed the
// marked form through two newly-parseable syntaxes, and this fixes the unmarked
// form per §28.6's leak-prevention rule).
//
// Spec: §28.6 (Leak prevention — owned-value disposal at scope end), §28.10
// (using var scope-end disposal), §28.3 (ownership classification of locals).
// Mirror of #1962 (which fixed the marked case).
//
// Codegen-observable, LeakSanitizer/AddressSanitizer-checked (runAotChecked).
// Each test pins: an owned-interface-typed parameter (or owned-interface-typed
// local) bound into an unmarked, unannotated `using var y := x;` disposes the
// resource EXACTLY ONCE on the scope-end path. The static net-live counter
// (ctor +1 / dtor -1) is the precision oracle (dispose exactly once => net 0;
// leak => +1; double-free => -1), and runAotChecked additionally fails on any
// leak/double-free/UAF the counter alone would not catch.
//
// FAIL-WITHOUT: 16-byte LSan leak (one Cell allocation never reclaimed).
// PASS-WITH: Cell.live == 0, ASan/LSan-clean.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Shared interfaces and classes — interface-typed owned bindings (the only
// kind that exercises the new branch; class-typed sources go through the
// static-disposal path and never reach the dynamic classifier).
const char* kCell =
    "module M; "
    "interface IThing method Ping(): int; end IThing; "
    "class Cell: IThing "
    "  public static live: int := 0; "
    "  public constructor() Cell.live := Cell.live + 1; end constructor; "
    "  public method Ping(): int return 1; end Ping; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// #1972 CORE — unmarked `using var y := x;` over an `owned` interface-typed
// PARAMETER. The unmarked adoption is treated as a transfer by the ownership
// pass (handleLocalDecl: `using var` over an owned source moves the obligation
// out of `x`, `x` is left moved-from, `y` is the sole owner); codegen must
// register a single dynamic dispose on `y` at scope end. The dynamic-dispose
// classifier's bare-id branch (mirroring #1962's a1, here without the marker)
// dispatches through vtable slot 2 of the concrete class held in the
// interface-typed slot.
// FAIL-WITHOUT: LSan 16-byte leak (one Cell allocation).
// PASS-WITH: Cell.live == 0, ASan/LSan-clean (exit == 0).
// ===========================================================================
BEZEL_TEST(using_var_interface_owned_1972, unmarked_param_adopts_interface_28_6) {
    auto r = runAotChecked(
        std::string(kCell) +
        "class Program "
        "  public static method Take(owned x: IThing): int "
        "    using var y := x; "          // unmarked, unannotated; per §28.6 must dispose
        "    var p := y.Ping(); "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    var r := Program.Take(move w); "   // §22.3 call-site transfer
        "    return Cell.live; "                 // 0 — disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan-clean (no leak, no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1972 CORE — unmarked `using var y := x;` over an `owned` interface-typed
// LOCAL. The source is a same-scope local (the named-owned-binding row of
// §28.10). Both ownership pass (handleLocalDecl) and codegen (the new branch
// in usingVarOwnsClassObjectDynamically) cooperate: the source is moved-from,
// the new binding owns, the dynamic dispose fires once. Wrapped in a
// `do ... end` block so the scope-end disposal runs before the `return` (the
// Main method's top-level scope-end disposal is a separate, pre-existing
// limitation not in scope here).
// FAIL-WITHOUT: LSan 16-byte leak.
// PASS-WITH: Cell.live == 0, ASan/LSan-clean (exit == 0).
// ===========================================================================
BEZEL_TEST(using_var_interface_owned_1972, unmarked_local_adopts_interface_28_6) {
    auto r = runAotChecked(
        std::string(kCell) +
        "class Program "
        "  public static method Main(): int "
        "    do "
        "      var x: IThing := new Cell(); "   // x owns an IThing (concrete Cell)
        "      using var y := x; "               // unmarked, unannotated; adopt x
        "      var p := y.Ping(); "
        "      _ := p; "
        "    end; "
        "    return Cell.live; "               // 0 — disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1972 CONTROL — annotated analog (already worked via the (a) branch of
// `usingVarOwnsClassObjectDynamically`). Verifies the fix does not regress the
// existing path that has handled the interface-annotation case since #383.
// `using var y: IThing := x;` — the dynamic dispose still fires exactly once.
// PASS-WITH and WITHOUT: Cell.live == 0, exit == 0. Guards against a
// regression in the existing annotated path.
// ===========================================================================
BEZEL_TEST(using_var_interface_owned_1972, annotated_param_control_28_6) {
    auto r = runAotChecked(
        std::string(kCell) +
        "class Program "
        "  public static method Take(owned x: IThing): int "
        "    using var y: IThing := x; "  // annotated; works via (a) branch
        "    var p := y.Ping(); "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    var r := Program.Take(move w); "
        "    return Cell.live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1972 CONTROL — marked analog (fixed by #1962). Verifies the existing
// marked-adoption path is unaffected by the unmarked-branch addition.
// `using var y := owned x;` — the dynamic dispose still fires exactly once.
// PASS-WITH and WITHOUT: Cell.live == 0, exit == 0.
// ===========================================================================
BEZEL_TEST(using_var_interface_owned_1972, marked_param_control_28_6_1962) {
    auto r = runAotChecked(
        std::string(kCell) +
        "class Program "
        "  public static method Take(owned x: IThing): int "
        "    using var y := owned x; "     // marked; works via (a1) branch (#1962)
        "    var p := y.Ping(); "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    var r := Program.Take(move w); "
        "    return Cell.live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1972 PRECISION — double-dispose guard. With the fix, exactly ONE dispose
// fires for `using var y := x;` over an `owned` interface-typed parameter.
// The double-dispose scenario (codegen registering on BOTH the source and
// the binding) would trip ASan as a use-after-free and flip r.ran to false,
// while a non-disposed scenario leaves Cell.live == 1. Both observed as
// failure. The net-live counter confirms exactly-once (Cell.live == 0).
// ===========================================================================
BEZEL_TEST(using_var_interface_owned_1972, exactly_once_no_double_dispose_28_6) {
    auto r = runAotChecked(
        std::string(kCell) +
        "class Program "
        "  public static method Take(owned x: IThing): int "
        "    using var y := x; "
        "    var p := y.Ping(); "
        "    return p; "
        "  end Take; "
        "  public static method Main(): int "
        "    var w := new Cell(); "
        "    var r := Program.Take(move w); "
        "    return r + Cell.live; "       // 1 (Ping return) + 0 = 1
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);       // dispose fires exactly once
}