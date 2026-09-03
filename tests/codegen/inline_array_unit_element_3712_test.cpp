// fix #3712 — the SAME `#1386` ICE shape #3528 fixed at the LOCAL-DECL
// collection-literal store loop reappears, unfixed, at THREE sibling loops: a
// STATIC field, an INSTANCE field, and a `return` statement, all storing a
// unit-typed (`()`) inline-array literal (spec §3.2 *Reference vs. value
// types*, §15.1 *Inline arrays*, §17.11 *The `default` operator* — v0.286).
//
// MEASURED on this branch pre-fix (the issue's own repros, `bezelc … ; echo
// $?`):
//
//   static field (repro b):
//     y7.bzl:3:42: internal: lowerExpr returned null for static field
//       inline-array initializer element — #1386          (x3, one per elem)
//     rc=1
//   instance field (repro a):
//     y5.bzl:3:25: internal: lowerExpr returned null for instance field
//       inline-array initializer element — #1386          (x3, one per elem)
//     rc=1
//   return statement (repro c):
//     y12.bzl:4:13: internal: lowerExpr returned null for inline-array
//       return element — #1386                            (x2, one per elem)
//     rc=1
//
// None of these SIGILL (unlike #3528's LOCAL/PARAMETER crash) — they
// diagnose at `Severity::Error` and exit 1 — but a bogus INTERNAL-ERROR
// diagnostic on a legal program (§15.1/§3.2/§17.11 all admit `[N]()`, per
// #3528's own spec reading) is the wrong outcome under LANE_RULES §6c just
// as much as a silent wrong answer would be.
//
// THE FIX hoists #3528's local-decl guard into ONE helper,
// `Emitter::lowerCollectionLiteralElementOrUnitNull` (declared
// LlvmBackend_impl.hpp, defined LlvmBackend_Setup.cpp immediately before
// `lowerLocalDecl`), keyed on the ELEMENT'S OWN SEMA TYPE being
// `Kind::Unit` — never on `lowerExpr` returning null, so a genuinely failed
// lowering of a NON-unit element still reaches the pre-existing #1386 ICE
// at all four sites. Each of the four collection-literal element-store
// loops (local decl, static field, instance field, return statement) now
// reassigns `ev` from the helper's placeholder-zero return and falls
// through its OWN unchanged per-element store code (GEP address + int-
// coercion + any site-specific wrap) rather than re-deriving that store
// logic a fifth time; the placeholder's LLVM type is always exactly
// `elemTy` by construction, so every one of those steps (int-coercion,
// `coerceToNullableSlot`, the static-field owned-pointer registration) is a
// verified no-op for it.
//
// §6b EXCLUSION DIFF (LANE_RULES §6b — mandatory for a unification lane):
// each of the four copies previously excluded EVERY unit-typed literal
// element (all four ICE'd/SIGILL'd unconditionally, since none tested the
// element's own sema type before treating a null `ev` as failure) except
// the local-decl one, which #3528 already admitted. After this fix, all
// four admit ONLY a unit-typed element (`et->kind == Kind::Unit`); a
// non-unit element whose `lowerExpr` genuinely returns null still hits the
// same #1386 diagnostic at its own site, unchanged — MEASURED via T5 below
// (an unrelated legal non-unit inline array at all three new sites, both
// sides of the fix, unaffected).
//
// SURFACE VALIDATED (runAotChecked — ASan/LSan net, since the static/
// instance-field sites also touch the §22.13 owned-slot registration path
// — MEASURED, all correct) at EACH of the three sibling sites: `Length`,
// indexed READ (`a[1]` yielding `()`), `for..in` iteration, structural `=`,
// and the array's own `.ToString()` (§8.5: "the type's name -- `[3]int`-
// style", so `[3]()` renders `"[3]()"`, not an element listing). T4 also
// exercises `lowerLocalDecl`'s refactored call to the SAME helper, pinning
// that hoisting it did not change #3528's own local-decl behavior.
//
//   FAIL-WITHOUT (src/ restored to the pre-fix parent 541bb33 via a scratch
//     checkout, this test file kept, BEZEL_AOT_CACHE=0):
//       T1 (static field)    0 passed, 1 failed  (r.ran == false, rc=1)
//       T2 (instance field)  0 passed, 1 failed  (r.ran == false, rc=1)
//       T3 (return)          0 passed, 1 failed  (r.ran == false, rc=1)
//       T4 (local, control)  1 passed, 0 failed  (already fixed by #3528)
//       T5 (non-unit control) 1 passed, 0 failed (unaffected either way)
//   PASS-WITH (bezelc relinked from the restored fix): 5 passed, 0 failed
//     (bezel_tests_codegen).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ── T1 — STATIC FIELD, the issue's repro (b). A `static readonly` field of
// inline-array type additionally takes the (unrelated, non-fatal) §22.12
// rule-4 "reachable from every domain" WARNING — that warning does not fail
// compilation and runAotChecked discards stderr on a successful compile
// (LANE_RULES §3), so it is invisible here; only the #1386 ICE (an Error)
// blocked compilation pre-fix.
BEZEL_TEST(codegen_inline_array_unit_element_3712,
           unit_element_inline_array_static_field_literal_spec_3_2_15_1) {
    auto r = runAotChecked(
        "module M3712A; "
        "class C "
        "  public static readonly arr: [3]() := [(), (), ()]; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.arr.Length); "
        "    Console.WriteLine(C.arr[1].ToString()); "
        "    var n: int := 0; "
        "    for var x in C.arr do "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "    Console.WriteLine(C.arr[0] = C.arr[1]); "
        "    Console.WriteLine(C.arr.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3\n()\n3\nTrue\n[3]()\n"));
}

// ── T2 — INSTANCE FIELD, the issue's repro (a).
BEZEL_TEST(codegen_inline_array_unit_element_3712,
           unit_element_inline_array_instance_field_literal_spec_3_2_15_1) {
    auto r = runAotChecked(
        "module M3712B; "
        "class C "
        "  public arr: [3]() := [(), (), ()]; "
        "  public static method Main(): int "
        "    var c: C := new C(); "
        "    Console.WriteLine(c.arr.Length); "
        "    Console.WriteLine(c.arr[1].ToString()); "
        "    var n: int := 0; "
        "    for var x in c.arr do "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "    Console.WriteLine(c.arr[0] = c.arr[1]); "
        "    Console.WriteLine(c.arr.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3\n()\n3\nTrue\n[3]()\n"));
}

// ── T3 — RETURN STATEMENT, the issue's repro (c). The returned inline array
// is bound to a fresh local (`var a := Make()`) so the surface below reads
// through the by-value copy the return produced, not the callee's own frame.
BEZEL_TEST(codegen_inline_array_unit_element_3712,
           unit_element_inline_array_return_statement_literal_spec_3_2_15_1) {
    auto r = runAotChecked(
        "module M3712C; "
        "class C "
        "  public static method Make(): [3]() "
        "    return [(), (), ()]; "
        "  end Make; "
        "  public static method Main(): int "
        "    var a := Make(); "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a[1].ToString()); "
        "    var n: int := 0; "
        "    for var x in a do "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "    Console.WriteLine(a[0] = a[1]); "
        "    Console.WriteLine(a.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3\n()\n3\nTrue\n[3]()\n"));
}

// ── T4 — LOCAL DECL, through the SAME hoisted helper (already fixed by
// #3528, whose own inline guard this fix replaced with a call to
// lowerCollectionLiteralElementOrUnitNull). Pins that the refactor did not
// change #3528's own local-decl behavior.
BEZEL_TEST(codegen_inline_array_unit_element_3712,
           unit_element_inline_array_local_literal_through_hoisted_helper_spec_3_2_15_1) {
    auto r = runAotChecked(
        "module M3712D; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]() := [(), (), ()]; "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a[1].ToString()); "
        "    var n: int := 0; "
        "    for var x in a do "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine(n); "
        "    Console.WriteLine(a[0] = a[1]); "
        "    Console.WriteLine(a.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3\n()\n3\nTrue\n[3]()\n"));
}

// ── T5 — BOTH-SIDES CONTROL, §6b's "what does the new admission NOT widen"
// check. Same three NEW sites (static field, instance field, return) with an
// ordinary `int` element instead of `()` — this already worked pre-fix (the
// old unconditional ICE only fired when `lowerExpr` returned null, which it
// never does for an `int` literal), and the helper's `Kind::Unit` gate means
// it is never even consulted here (the `if (!ev)` branch is not taken), so
// this row passes on BOTH sides of the fix and pins that the widening is
// exactly "a unit-typed element", nothing broader.
BEZEL_TEST(codegen_inline_array_unit_element_3712,
           control_non_unit_element_at_the_three_sibling_sites_unaffected_spec_15_1) {
    auto r = runAotChecked(
        "module M3712E; "
        "class C "
        "  public static readonly sArr: [3]int := [1, 2, 3]; "
        "  public iArr: [3]int := [4, 5, 6]; "
        "  public static method Make(): [3]int "
        "    return [7, 8, 9]; "
        "  end Make; "
        "  public static method Main(): int "
        "    var c: C := new C(); "
        "    var r := Make(); "
        "    Console.WriteLine(C.sArr[1]); "
        "    Console.WriteLine(c.iArr[1]); "
        "    Console.WriteLine(r[1]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n5\n8\n"));
}
