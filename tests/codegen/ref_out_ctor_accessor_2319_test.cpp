// Spec-derived tests for issue #2319 — accessor-mediated `ref`/`out` argument
// to a CONSTRUCTOR call (§22.3 bracket not wired to the ctor paths).
//
// §22.3 (v0.228) admits "an indexer slot, or a property with an accessible
// setter" as `ref`/`out` argument locations and gives them copy-in / copy-out
// semantics, with NO restriction to method calls — the same bracket applies
// to constructor calls (`new Ctor(ref p.X)` is admitted). The relevant §22.3
// passage (full quote in tests/codegen/ref_out_accessor_writeback_1302_test.cpp):
//
//   "A `ref` argument naming a mutable local or writable field is a true
//    alias: callee writes are visible through the caller's location
//    immediately … An **indexer slot or property** target is instead
//    **copy-in / copy-out** under §17.6's evaluate-once discipline: the
//    receiver and every index expression are evaluated exactly once, in
//    §22.5's call-site textual order; for `ref`, the getter is called once
//    to produce the callee's slot value (for `out` no getter call occurs);
//    on **normal return** the setter is called once with the slot's final
//    value."
//
// #1302 implemented the bracket on the method-call path (`lowerCall`) and
// gated it behind an opt-in `accessorRefArgs` sink parameter on
// `buildOrderedArgs` (#1302 commit message). The four constructor sites in
// `LlvmBackend_Exprs2.cpp` (early pre-pass, importedGenericWithBody, the
// imported ctor manual loop, the CG-generic inline ctor — plus the record
// body ctor in `buildRecordValue`) passed `nullptr`, leaving them with the
// #1386 fallback: an `internal: lowerExpr returned null for call argument in
// buildOrderedArgs — #1386` ICE on every accepted §22.3 program like
// `new Ctor(ref arr[0])`. The fix routes the sink through all four sites
// and flushes the copy-OUT (the setter call) once the constructor body/call
// has returned NORMALLY (matching `lowerCall`'s post-call discipline
// exactly). #2319.
//
// Pre-fix behavior (mechanically confirmed on the pre-fix compiler):
//   * `new Ctor(ref arr[0])` → #1386 ICE (compile error, .ran=false).
//   * `new Ctor(out arr[0])` → #1386 ICE (compile error, .ran=false).
//   * Plain-local / field `ref` / `out` (`tests/codegen/ctor_ref_param_967_test.cpp`)
//     continued to compile and run correctly — only accessor-mediated sites
//     were affected.
// Every accessor-mediated test below therefore fails on the PRE-FIX compiler
// (.ran=false, exitCode=-999 from the run() helper, or a non-15/99 value if
// the bracket were to silently drop the write). Each asserts the §22.3 value
// directly, mirroring the #1302 test file's pattern.
//
// `ctor_ref_param_967_test.cpp` covers the alias-path controls (`ref` / `out`
// on a plain local); those tests continue to pass — they assert the existing
// alias behavior, not the bracket.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) {
        // Surface diagnostics so a regression is not silently -999; the BEZEL
        // harness only prints the "expected equality" failure above otherwise.
        std::fprintf(stderr,
                     "DIAG (no .ran) for program of length %zu: %s\n",
                     prog.size(),
                     r.diagnostics.hasErrors() ? "has errors" : "no errors");
        if (r.diagnostics.hasErrors()) {
            for (const auto& d : r.diagnostics.diagnostics())
                std::fprintf(stderr, "  msg: %s\n", d.message.c_str());
        }
    }
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// =============================================================================
// The cardinal fix: accessor-mediated `ref` to a CONSTRUCTOR call.
// =============================================================================

// §22.3 / fix #2319: `new Bumper(ref arr[0])` — the bracket runs the getter
// once (copy-IN), the ctor body writes the slot, then on normal return the
// setter writes the slot back to `arr[0]`. Pre-fix: #1386 ICE. Post-fix:
// 10 + 5 = 15.
BEZEL_TEST(ref_out_ctor_accessor_2319, ref_to_heap_array_slot_in_ctor_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Bumper "
        "  public constructor(ref a: int) a := a + 5; end constructor; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); "
        "    using var b := new Bumper(ref arr[0]); "
        "    return arr[0]; "
        "  end Main; "
        "end C;"), 15);
}

// §22.3 / fix #2319: the `out` sibling — "for `out` no getter call occurs",
// then the setter runs on normal return. Pre-fix: #1386 ICE. Post-fix: 99.
BEZEL_TEST(ref_out_ctor_accessor_2319, out_to_heap_array_slot_in_ctor_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Setter "
        "  public constructor(out a: int) a := 99; end constructor; "
        "end Setter; "
        "public class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(1); "
        "    using var b := new Setter(out arr[0]); "
        "    return arr[0]; "
        "  end Main; "
        "end C;"), 99);
}

// =============================================================================
// Alias-path controls — already worked pre-fix; pin them so a future refactor
// doesn't regress them into the bracket.
// =============================================================================

// §22.3: `ref` to a plain local in a CONSTRUCTOR call — a true alias; no
// bracket needed. (Covered in detail by tests/codegen/ctor_ref_param_967_test.cpp,
// pinned here so a ctor-specific regression does not slip through.)
BEZEL_TEST(ref_out_ctor_accessor_2319, ref_to_local_in_ctor_still_aliases) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Bumper "
        "  public constructor(ref a: int) a := a + 5; end constructor; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    using var b := new Bumper(ref x); "
        "    return x; "
        "  end Main; "
        "end C;"), 15);
}

// §22.3: the `out` sibling for the alias path. x is uninitialized at the
// call site but `out` does not require pre-assignment (§22.4: the inline
// recipient is set by the callee).
BEZEL_TEST(ref_out_ctor_accessor_2319, out_to_local_in_ctor_still_writes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Setter "
        "  public constructor(out a: int) a := 99; end constructor; "
        "end Setter; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 1; "
        "    using var b := new Setter(out x); "
        "    return x; "
        "  end Main; "
        "end C;"), 99);
}

// =============================================================================
// Multi-arg: accessor-mediated `ref` mixed with an ordinary argument.
// =============================================================================

// §22.3 / fix #2319: a multi-arg ctor with one accessor-mediated `ref` arg
// and one ordinary string arg. The bracket fires for the slot; the ordinary
// arg uses the value path. arr[0] gets +5 (10 -> 15); arr[1] is untouched.
// 15 + 20 = 35. Pre-fix: #1386 ICE. Post-fix: 35.
BEZEL_TEST(ref_out_ctor_accessor_2319, ref_accessor_with_ordinary_arg_in_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Bumper "
        "  public constructor(ref a: int, msg: string) a := a + 5; "
        "  end constructor; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); arr.Add(20); "
        "    using var b := new Bumper(ref arr[0], \"ok\"); "
        "    return arr[0] + arr[1]; "
        "  end Main; "
        "end C;"), 35);
}

// =============================================================================
// Property accessor — the other §22.3 shape (a property with an accessible
// setter).
// =============================================================================

// §22.3 / §23.3 / fix #2319: a property target reached through the ctor
// bracket. The ctor body writes `a + 5` into the slot, then on normal
// return the setter runs (the write-back). Pre-fix: #1386 ICE. Post-fix: 15.
BEZEL_TEST(ref_out_ctor_accessor_2319, ref_to_property_in_ctor_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  private _v: int; "
        "  public constructor() this._v := 10; end constructor; "
        "  public property V: int "
        "    get => this._v; "
        "    set => this._v := value; "
        "  end V; "
        "end Box; "
        "public class Bumper "
        "  public constructor(ref a: int) a := a + 5; end constructor; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    using var bx := new Box(); "
        "    using var bp := new Bumper(ref bx.V); "
        "    return bx.V; "
        "  end Main; "
        "end C;"), 15);
}

// =============================================================================
// Dictionary indexer slot — a different heap-collection accessor shape.
// =============================================================================

// §22.3 / stdlib §4.8 / fix #2319: a `Dictionary<K, V>` indexer slot,
// bracketed in the ctor. The bracket drives the dictionary's own key lookup
// on both the getter (copy-IN) and the setter (copy-OUT). 10 + 5 = 15.
BEZEL_TEST(ref_out_ctor_accessor_2319, ref_to_dictionary_slot_in_ctor_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Bumper "
        "  public constructor(ref a: int) a := a + 5; end constructor; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new Dictionary<string, int>(); d[\"k\"] := 10; "
        "    using var b := new Bumper(ref d[\"k\"]); "
        "    return d[\"k\"]; "
        "  end Main; "
        "end C;"), 15);
}

// =============================================================================
// Adversarial: the bracket's stated discipline applied to the ctor path.
// =============================================================================

// §22.3 / §17.6 (fix #2319): the receiver / index expressions are evaluated
// exactly once. A write-back that re-lowered the index would call Idx()
// twice — the #1299 hazard for `a[i] OP= v`. arr[1] must be 25 and Calls
// must be 1, so 25 + 1 = 26. A double evaluation would give 27; a dropped
// write-back would give 21. Confirms evaluate-once is wired into the ctor
// path too.
BEZEL_TEST(ref_out_ctor_accessor_2319, index_evaluated_exactly_once_in_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Bumper "
        "  public static Calls: int := 0; "
        "  public constructor(ref a: int) a := a + 5; end constructor; "
        "  public static method Idx(): int "
        "    Bumper.Calls := Bumper.Calls + 1; return 1; "
        "  end Idx; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); arr.Add(20); arr.Add(30); "
        "    using var b := new Bumper(ref arr[Bumper.Idx()]); "
        "    return arr[1] + Bumper.Calls; "
        "  end Main; "
        "end C;"), 26);
}

// §22.3 (fix #2319): "on an exception exit no write-back occurs" — the
// same edge the `out` copy-out rule states. The ctor assigns 77 through
// the `ref` and then throws; the caught path must observe the ORIGINAL 10,
// not the 77 (which would have leaked into `arr[0]` via the write-back).
BEZEL_TEST(ref_out_ctor_accessor_2319, no_write_back_on_exception_in_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Bumper "
        "  public constructor(ref a: int) "
        "    a := 77; throw new InvalidOperationException(\"x\"); "
        "  end constructor; "
        "end Bumper; "
        "public class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); "
        "    try "
        "      using var b := new Bumper(ref arr[0]); "
        "    catch e: InvalidOperationException => return arr[0]; end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 10);
}
