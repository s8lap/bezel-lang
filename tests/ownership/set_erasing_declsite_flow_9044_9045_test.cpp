// Regression tests for issues #9044 and #9045 — §28.3 closure bullet
// *Provenance* clauses 1, 3 and 6 and *Set-erasing positions* (v0.359): the two
// gaps the non-author review of #8963 left open.
//
// THE DEFECTS.  #8963 made a lambda's plain function-typed parameter
// call-bounded inside a FIELD INITIALIZER, but its declaration-site walk reads
// only DIRECT uses of the parameter:
//
//   #9044  a BLOCK-bodied lambda in a field initializer that routes the
//          parameter through a LOCAL (`var f: () -> int := q; rg.Keep(f);`)
//          was admitted: the local had no flow facts, so it read as UNTRACED.
//          The byte-identical lambda in a METHOD BODY is rejected, and under
//          ASan the admitted program is a heap-use-after-free.
//   #9045  an EXPRESSION-bodied METHOD's own plain function-typed parameter
//          (`method M(rg: Reg2, q: () -> int) => rg.Keep(q)`) was never
//          seeded call-bounded: `run` seeds a block body's formals, the `=>`
//          path did not.  The block-bodied twin is rejected.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*:
//
//   clause 3: "A plain function-typed parameter of the enclosing body
//   contributes itself: its signature announces no set (K2.4), so the value is
//   bounded by the call, §28.14's call-bounded tier."
//   clause 1: a local "contributes, at each read, the union of the sets of the
//   forms the flow analysis traces it to" — a local initialised from a
//   call-bounded value is call-bounded.
//
// and *Set-erasing positions*: "At every set-erasing position a function value
// with a non-empty source set is rejected".
//
// ROWS.  Every row is RED on the base tree (the program compiles).
// ORACLE.  `runAot`: the expected outcome is a REJECTION with the specific
// set-erasing diagnostic at a real location.

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

// The #8963 / #9044 harness: `Reg2.Keep(o: Object)` stores the value and
// `RunInt()` invokes it after the call that passed it returned. `members` is
// placed in `Fl`; `useStmt` is the statement in `Mk` that exercises it.
std::string registryProgram(const std::string& members,
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
           "  public method Keep(o: Object) _o := o; end Keep; "
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
           "  public static method Mk(reg: Reg2, r: Res) "
           "    var fl := new Fl(); " +
           useStmt +
           "  end Mk; "
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); "
           "        Mk(reg, r); "
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// REJECT — #9044's repro: a block-bodied lambda in an INSTANCE field's
// initializer aliases its call-bounded parameter through a local and passes the
// local at `Reg2.Keep(o: Object)`.  RED on the base tree (compiled; ASan
// heap-use-after-free in `Reg2.RunInt`).
BEZEL_TEST(set_erasing_declsite_flow_9044_9045,
           block_lambda_in_field_initializer_local_alias_at_object_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public H: (Reg2, () -> int) -> () := "
        "lambda(rg: Reg2, q: () -> int) => do "
        "  var f: () -> int := q; "
        "  rg.Keep(f); "
        "end; ",
        "fl.H(reg, lambda() => r.V);"));
    expectSetErasingReject(r);
}

// REJECT — #9044's static-field twin, through a CHAIN of two locals.
BEZEL_TEST(set_erasing_declsite_flow_9044_9045,
           static_field_block_lambda_local_chain_at_object_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public static readonly H: (Reg2, () -> int) -> () := "
        "lambda(rg: Reg2, q: () -> int) => do "
        "  var g: () -> int := q; "
        "  var f: () -> int := g; "
        "  rg.Keep(f); "
        "end; ",
        "Fl.H(reg, lambda() => r.V);"));
    expectSetErasingReject(r);
}

// REJECT — #9044: the alias is assigned (not declared) from the parameter on
// one branch; the assignment reaches the read (clause 6).
BEZEL_TEST(set_erasing_declsite_flow_9044_9045,
           block_lambda_in_field_initializer_reaching_assignment_at_object_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public H: (Reg2, () -> int, bool) -> () := "
        "lambda(rg: Reg2, q: () -> int, c: bool) => do "
        "  var f: () -> int := lambda() => 9; "
        "  if c then f := q; end if; "
        "  rg.Keep(f); "
        "end; ",
        "fl.H(reg, lambda() => r.V, true);"));
    expectSetErasingReject(r);
}

// REJECT — #9045's repro: an EXPRESSION-bodied method's own plain
// function-typed parameter at `Reg2.Keep(o: Object)`.  RED on the base tree
// (compiled).
BEZEL_TEST(set_erasing_declsite_flow_9044_9045,
           expression_bodied_method_own_fn_parameter_at_object_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public method M(rg: Reg2, q: () -> int) => rg.Keep(q); ",
        "fl.M(reg, lambda() => r.V);"));
    expectSetErasingReject(r);
}

// REJECT — #9045: the same through a STATIC expression-bodied method.
BEZEL_TEST(set_erasing_declsite_flow_9044_9045,
           static_expression_bodied_method_own_fn_parameter_at_object_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public static method M(rg: Reg2, q: () -> int) => rg.Keep(q); ",
        "Fl.M(reg, lambda() => r.V);"));
    expectSetErasingReject(r);
}
