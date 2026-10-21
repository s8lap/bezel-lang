// Codegen / runtime — §11 (properties) / §4.6 / §28.9 / #2304: an
// EXPRESSION-BODIED `string` PROPERTY read `obj.Tag` (a MemberAccessExpr, NOT
// a CallExpr) returns a +1 heap string the binding consumer (a `var s := obj.Tag;`
// local, the same role isUserMethodStringReturnCall covers for method-shape calls)
// must TRANSFER (no extra retain) and release at scope end if non-escaping.
//
// §11 / §22.9 (v0.236): a property may have an expression body — `property P: T
// => expr;` — the body is the getter. The property declares its own co-1 by
// construction: each read mints a fresh rc=1 if the body is a +1 producer shape
// (concatenation, interpolation, transforming method call, etc.).
//
// §4.6 / §28.9 (v0.236): every +1 string producer's reference must reach a
// transfer-and-release pair — borrowing instead of transferring double-counts
// the reference and leaks the producer's original +1.
//
// #1934 closed the same gap for a `case`/`when`-expression result bound to a
// local. #2199 closed the same gap for transforming instance-method calls
// (`s.ToUpper()`, `s.Substring(...)`, etc.) bound to a local. The PROPERTY
// shape was the residual — isPlusOneStringProducer's MethodSymbol / closure
// call arms did not admit a MemberAccessExpr whose resolved symbol is a
// PropertySymbol, so a `var s := obj.Tag;` local retained the result as a
// BORROW and never discharged the producer's +1. The same shape written as a
// method (`public method Tag(): string return "tag" + "!"; end Tag;`) is
// LSan-clean (isUserMethodStringReturnCall admits the user-method call).
//
// THE FIX (#2304). isPlusOneStringProducer now has a MemberAccessExpr arm
// that, for a NON-INDEXER string property (the syntax is `obj.Tag`, not
// `obj[i]`), checks the property's expression body via the SAME
// `computedGetterOf` predicate lowerPropertyRead's CG33b keys on — an
// expression-bodied property (`=>` form) OR a block-getter with an
// expression body (`{ get => expr; }`). If the body is itself a +1 producer
// (recognized via the existing arms — `+` / interpolation / case / if /
// user-method string return / closure returning string / ToString family /
// string.Join / string-substring family), the property read is admitted as
// a +1 producer; otherwise it is conservatively excluded (the safe
// direction — a leak, never a double-free).
//
// OBSERVABILITY. The leak is a DIRECT leak of the freshly-allocated bz.string
// header + its data buffer (the producer's +1, with no other reference
// holding it after the local's scope end). runAotChecked's LSan flags it:
// WITHOUT the fix `r.ran` is false (Direct+Indirect leak of the bz.string
// header + data buffer per iteration); WITH it the run is clean AND no
// double-free / UAF is introduced (the dispose-exactly-once net rides on
// `r.ran`).
//
// TEST SHAPE. The cardinal repro from the issue (`var s := a.Tag;` then
// `s.Length`). The method-shape sibling `Tag(): string` is included as a
// regression companion — it stays clean already (proves the fix did not
// regress the user-method path) and is the negative control mirroring the
// issue's note.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// §11 / §4.6 / §28.9 / #2304: the cardinal repro. An expression-bodied
// `string` property `Tag => "tag" + "!"` is read in a loop, the result is
// bound to `var s := a.Tag;`, then `s.Length` is observed. 100 iterations
// amplify the leak (37 bytes / 2 allocations per read — the bz.string
// header + its 5-byte data buffer). The method-shape companion `Tag(): string`
// (a separate `class Api2`) runs in the same loop as a regression check on
// the still-clean user-method path. The exit code is `sink mod 256` — the
// program's own observable value (independent of the leak).
const char* kStringPropertyLoop =
    "module M; "
    "class Api "
    "  public constructor() end constructor; "
    "  public property Tag: string => \"tag\" + \"!\"; "   // EXPR body → +1
    "  destructor end destructor; "
    "end Api; "
    "class Api2 "
    "  public constructor() end constructor; "
    "  public method Tag(): string "
    "    return \"tag\" + \"!\"; "                         // METHOD → +1
    "  end Tag; "
    "  destructor end destructor; "
    "end Api2; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    using var a := new Api(); "
    "    var s := a.Tag; "                              // the #2304 shape
    "    sink := sink + s.Length; "
    "    using var a2 := new Api2(); "
    "    var s2 := a2.Tag(); "                          // method (already clean)
    "    sink := sink + s2.Length; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §11 / §4.6 / §28.9 / #2304 variant — an accessor-block getter with an
// expression body (`get => expr;`) must be admitted the same way; the body
// shape is a `+` concatenation, recognized via the existing arms in
// isPlusOneStringProducer. Mirrors `computedGetterOf`'s accessor expression
// branch so the producer / lowering stay in lockstep.
const char* kStringAccessorPropertyLoop =
    "module M; "
    "class Api "
    "  public constructor() end constructor; "
    "  public property Tag: string "
    "    get => \"x\" + \"y\"; "
    "  end Tag; "
    "  destructor end destructor; "
    "end Api; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    using var a := new Api(); "
    "    var s := a.Tag; "
    "    sink := sink + s.Length; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// #2304 — an expression-bodied `string` PROPERTY read bound to a local is
// released at scope end (§11 / §4.6 / §28.9); no leak. The cardinal repro.
// =====================================================================

BEZEL_TEST(codegen_string_property_call_local_leak_2304,
           string_property_expr_body_local_does_not_leak_spec_11_4_6_28_9) {
    // spec §11 / §4.6 / §28.9, v0.236: a property whose expression body mints
    // a +1 string (`+` / interpolation / transforming call) hands the caller
    // exactly +1 — the consumer TRANSFERS the reference (no extra retain)
    // and the escape walker proves the local non-escaping (only safe reads —
    // member access on `s`) so the scope-end bz_string_release drops rc 1→0.
    // WITHOUT the #2304 fix isPlusOneStringProducer did not recognize the
    // MemberAccessExpr → PropertySymbol shape (its arms only covered
    // CallExpr callees), so the local was treated as a BORROW: rc went
    // 1→2 at bind and 2→1 at scope end — never dropping the producer's
    // original +1 → 3700 bytes leaked total over 100 iterations (37
    // bytes × 2 [property + method] per iter; but here only the property
    // arm leaks). WITH the fix the property is admitted as a +1 producer
    // and the local's scope end releases the header + data buffer.
    JitResult r = runAotChecked(kStringPropertyLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2304 variant — accessor-block getter with an expression body (the
// `get => expr;` shape). Same release shape; this variant guards the
// dual branch of computedGetterOf in lockstep with the lowering.
// =====================================================================

BEZEL_TEST(codegen_string_property_call_local_leak_2304,
           string_accessor_property_expr_body_does_not_leak_spec_11_4_6_28_9) {
    JitResult r = runAotChecked(kStringAccessorPropertyLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);
}
