// Spec-derived tests for issue #2588 — a `ref`/`out` parameter on a GENERIC
// class's EXPRESSION-BODIED method was bound in the inlined method body via
// `paramSlots_`, so a body read loaded the incoming POINTER itself instead
// of the value it points at, emitting malformed IR
// (`add ptr, i32` — "Both operands to a binary operator are not of the same
// type!") that `verifyModule` rejects.
//
// §22.3 (v0.236): a `ref`/`out` parameter aliases the caller's storage; reads
// and writes of the parameter go through that pointer. The compiled-function
// path registers the incoming address in `refParams_` (bindParameters,
// CG50), so a body read dereferences correctly (lowerExpr's IdentifierExpr
// arm checks refParamAddr/refParams_ BEFORE localSlot/paramSlots_). The
// inline path for a GENERIC EXPRESSION-BODIED method
// (emitInlineGenericMethod, LlvmBackend_Calls2.cpp) bound EVERY param through
// `paramSlots_`, which for a ref/out param stores the address and then reads
// it back as the value — the body saw the pointer. This is the METHOD-path
// sibling of #2283 (generic-class CONSTRUCTOR inline path) and #2579
// (chain-call inline path), both now landed.
//
// Block-bodied generic methods are unaffected: they go through
// emitGenericSpecialization -> buildSpecializedMethodFn -> bindParameters
// (which already routes ref/out through refParams_), covered by
// codegen_generic.ref_param_in_spec_inorder. Only the EXPRESSION-BODIED
// inline path had this gap: expression bodies are inlined directly via
// lowerExpr inside emitInlineGenericMethod and never reach bindParameters.
//
// The fix registers a ref/out param's address in `refParams_` (with
// save/restore of the caller's map) at the expression-bodied method inline
// site — exactly what bindParameters does for the compiled path, what #2283
// did for the ctor inline paths, and what #2579 did for the chain-call inline
// path. §22.3 (v0.236) puts no restriction on `ref` in a method's parameter
// list, and an expression-bodied method is a legal declaration form (§22/§9).
//
// Pre-fix behavior (mechanically confirmed on origin/main, commit f360b6e):
// every generic-class expression-bodied ref/out method case below failed
// with a malformed-IR build error (`.ran=false`, `run() == -999`); the
// program never ran, so no wrong value was produced. The value-param,
// non-generic, and block-bodied controls compile either way.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) {
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
// The cardinal fix (issue #2588 repro): a `ref` param on a GENERIC class's
// EXPRESSION-BODIED method. Pre-fix: malformed IR (the body loaded the ptr).
// Post-fix: the alias is dereferenced, returning n + 1 = 11.
// =============================================================================

// §22.3 / fix #2588: the issue's exact shape — `Box<int>.Bump(ref n)` with an
// expression body `=> n + 1`. n starts at 10; the body READS n through the
// alias and returns n + 1. Pre-fix: `add ptr, i32` malformed IR, run()==-999.
BEZEL_TEST(ref_out_generic_expr_method_2588, generic_expr_method_ref_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public constructor() end constructor; "
        "  public method Bump(ref n: int): int => n + 1; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    var n: int := 10; "
        "    var r := b.Bump(ref n); "
        "    return r; "
        "  end Main; "
        "end C;"), 11);
}

// §22.3 / fix #2588: a GENERIC-TYPED ref param — `Read(ref x: K): K => x`.
// Confirms the alias carries the caller's value IN through a ref param whose
// type is the class's own type parameter (K = int at the instantiation).
// n=42 → returns 42. Pre-fix: malformed IR.
BEZEL_TEST(ref_out_generic_expr_method_2588,
           generic_expr_method_ref_generic_typed_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public constructor() end constructor; "
        "  public method Read(ref x: K): K => x; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    var n: int := 42; "
        "    return b.Read(ref n); "
        "  end Main; "
        "end C;"), 42);
}

// §22.3 / fix #2588: a ref param AND a value param together in one
// expression-bodied generic method — `Add(ref a: int, b: int): int => a + b`.
// Confirms the ref param dereferences while the value param keeps its
// paramSlots_ binding. a=10 (ref), b=5 (value) → 15. Pre-fix: malformed IR.
BEZEL_TEST(ref_out_generic_expr_method_2588,
           generic_expr_method_ref_and_value_params) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public constructor() end constructor; "
        "  public method Add(ref a: int, b: int): int => a + b; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    var a: int := 10; "
        "    return b.Add(ref a, 5); "
        "  end Main; "
        "end C;"), 15);
}

// =============================================================================
// Controls — these compile/run EITHER way (they exercise paths the fix does
// not touch) and must stay green.
// =============================================================================

// Control / fix #2588: a VALUE param on a generic expression-bodied method
// (no ref). Value params keep the paramSlots_ binding the inline path always
// used, so this is unaffected by the fix. n=10 → 11 either way.
BEZEL_TEST(ref_out_generic_expr_method_2588,
           generic_expr_method_value_param_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public constructor() end constructor; "
        "  public method BumpV(n: int): int => n + 1; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.BumpV(10); "
        "  end Main; "
        "end C;"), 11);
}

// Control / fix #2588: a NON-GENERIC class's expression-bodied method with a
// ref param. A non-generic method is emitted as a COMPILED function (not via
// emitInlineGenericMethod), whose bindParameters already routes ref/out
// through refParams_ — so this compiled clean before #2588 and stays clean.
// n=10 → 11 either way.
BEZEL_TEST(ref_out_generic_expr_method_2588,
           non_generic_expr_method_ref_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public constructor() end constructor; "
        "  public method Bump(ref n: int): int => n + 1; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var n: int := 10; "
        "    return b.Bump(ref n); "
        "  end Main; "
        "end C;"), 11);
}

// Control / fix #2588: a BLOCK-BODIED generic method with a ref param. Block
// bodies go through emitGenericSpecialization -> buildSpecializedMethodFn ->
// bindParameters (which already routes ref/out through refParams_), so this
// compiled clean before #2588 and must stay clean. The sibling of
// codegen_generic.ref_param_in_spec_inorder, kept self-contained here.
// SetN writes 7 through the alias; returns 7 either way.
BEZEL_TEST(ref_out_generic_expr_method_2588,
           generic_block_method_ref_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public constructor() end constructor; "
        "  public method SetN(ref n: int) "
        "    n := 7; "
        "  end SetN; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    var n: int := 0; "
        "    b.SetN(ref n); "
        "    return n; "
        "  end Main; "
        "end C;"), 7);
}
