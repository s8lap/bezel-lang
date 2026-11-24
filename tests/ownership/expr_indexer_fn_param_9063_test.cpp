// Regression tests for issue #9063 — §28.3 closure bullet *Provenance*
// clause 3 and *Set-erasing positions* (v0.359).
//
// THE DEFECT.  #9045 seeded the plain function-typed formals of an
// EXPRESSION-bodied method / operator as call-bounded, but an INDEXER's own
// formals were never seeded: in `property [rg: Reg2, q: () -> int]: int =>
// rg.KeepI(q)` the read of `q` was UNTRACED and the program was admitted at
// `Reg2.KeepI(o: Object)`, which stores it.  The `=>` accessor bodies
// (`get => ...`, `set => ...`) had the same gap.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*, clause 3: "A plain
// function-typed parameter of the enclosing body contributes itself: its
// signature announces no set (K2.4), so the value is bounded by the call,
// §28.14's call-bounded tier."  *Set-erasing positions*: "At every set-erasing
// position a function value with a non-empty source set is rejected".
//
// ROWS.  Every reject row is RED on the base tree (the program compiles).  The
// admit row is a base-green control: an indexer that only INVOKES its
// function-typed parameter.
// ORACLE.  `runAot`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kCarriesBorrow = "function value carries captured borrow(s)";
const char* kAtObject =
    "may not be passed where the parameter is declared Object";

void expectSetErasingReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

std::string indexerProgram(const std::string& members,
                           const std::string& useStmt) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "end Res; "
           "public class Reg2 "
           "  private _o: Object? := null; "
           "  public constructor() end constructor; "
           "  public method KeepI(o: Object): int _o := o; return 1; end KeepI; "
           "  public method RunInt(): int "
           "    if _o is () -> int then return (_o as () -> int)!(); end if; "
           "    return 7; "
           "  end RunInt; "
           "end Reg2; "
           "public class Fl "
           "  " +
           members +
           "  public constructor() end constructor; "
           "end Fl; "
           "public class Program "
           "  public static method Mk(reg: Reg2, r: Res): int "
           "    var fl := new Fl(); " +
           useStmt +
           "  end Mk; "
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); "
           "        var n := Mk(reg, r); "
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// REJECT — #9063's repro: an expression-bodied indexer's own function-typed
// parameter at `Reg2.KeepI(o: Object)`.  RED on the base tree.
BEZEL_TEST(expr_indexer_fn_param_9063,
           expression_bodied_indexer_own_fn_parameter_at_object_rejected_spec_28_3) {
    auto r = runAot(indexerProgram(
        "public property [rg: Reg2, q: () -> int]: int => rg.KeepI(q); ",
        "return fl[reg, lambda() => r.V];"));
    expectSetErasingReject(r);
}

// REJECT — the `get => E` accessor form of the same indexer.
BEZEL_TEST(expr_indexer_fn_param_9063,
           expression_bodied_indexer_getter_own_fn_parameter_at_object_rejected_spec_28_3) {
    auto r = runAot(indexerProgram(
        "public property [rg: Reg2, q: () -> int]: int "
        "  get => rg.KeepI(q); "
        "end; ",
        "return fl[reg, lambda() => r.V];"));
    expectSetErasingReject(r);
}

// REJECT — the `set => E` accessor form: the indexer's own parameter is in
// scope there too.
BEZEL_TEST(expr_indexer_fn_param_9063,
           expression_bodied_indexer_setter_own_fn_parameter_at_object_rejected_spec_28_3) {
    auto r = runAot(indexerProgram(
        "public property [rg: Reg2, q: () -> int]: int "
        "  get => 1; "
        "  set => rg.KeepI(q); "
        "end; ",
        "fl[reg, lambda() => r.V] := 1; return 0;"));
    expectSetErasingReject(r);
}

// ADMIT (base-green control) — the indexer only INVOKES its function-typed
// parameter; a call-bounded value is fine there (§28.14).
BEZEL_TEST(expr_indexer_fn_param_9063,
           expression_bodied_indexer_fn_parameter_only_invoked_admitted_spec_28_3) {
    auto r = runAot(indexerProgram(
        "public property [q: () -> int]: int => q(); ",
        "return fl[lambda() => r.V];"));
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
}
