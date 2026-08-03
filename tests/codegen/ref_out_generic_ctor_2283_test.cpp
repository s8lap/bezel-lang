// Spec-derived tests for issue #2283 — a `ref`/`out` parameter on a GENERIC
// class's CONSTRUCTOR (and the record-body ctor sibling) was bound in the
// inlined constructor body via `paramSlots_`, so a body read loaded the
// incoming POINTER itself instead of the value it points at, emitting
// malformed IR (`add ptr, i32` — "Both operands to a binary operator are not
// of the same type!") that `verifyModule` rejects.
//
// §22.3 (v0.236): a `ref`/`out` parameter aliases the caller's storage; reads
// and writes of the parameter go through that pointer. The compiled-function
// path registers the incoming address in `refParams_` (bindParameters,
// CG50), so a body read dereferences correctly. The INLINE ctor paths in
// `lowerNew` / `buildRecordValue` (the imported-generic-with-body ctor, the
// CG-generic inline ctor, and the record-body ctor — all in
// `LlvmBackend_Exprs2.cpp`) bound EVERY param through `paramSlots_`, which
// for a ref/out param stores the address and then reads it back as the value
// — the body saw the pointer. (The issue's note that "the same constructor on
// a NON-generic class compiles clean" is because a non-generic class ctor is
// emitted as a compiled function, whose bindParameters registers refParams_.)
//
// The fix registers a ref/out param's address in `refParams_` (with
// save/restore of the caller's map) at each inline ctor body-emission site —
// exactly what `bindParameters` does for the compiled path and what #2579's
// chain-call inline-path fix did. §22.3 puts no restriction on `ref` in a
// constructor's parameter list.
//
// Pre-fix behavior (mechanically confirmed on origin/main, commit 738035a):
// every generic-class ref/out ctor case below failed with a malformed-IR
// build error (`.ran=false`, `run() == -999`); the program never ran, so no
// wrong value was produced. The non-generic and value-param controls compile
// either way (the compiled-function path / the paramSlots_ path for values).

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
// The cardinal fix (issue #2283 repro): a `ref` param on a GENERIC class's
// constructor. Pre-fix: malformed IR (the body loaded the ptr). Post-fix: 1.
// =============================================================================

// §22.3 / fix #2283: the issue's exact repro — `new Box<int>(ref n)` with
// `n := n + 1` in the ctor body. n starts at 0, the ctor mutates it to 1
// through the alias. Pre-fix: `add ptr, i32` malformed IR, run()==-999.
BEZEL_TEST(ref_out_generic_ctor_2283, generic_class_ref_ctor_param_mutates) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  private _v: int := 0; "
        "  public constructor(ref n: int) n := n + 1; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    using var b := new Box<int>(ref n); "
        "    return n; "
        "  end Main; "
        "end C;"), 1);
}

// §22.3 / fix #2283: the generic ctor READS the ref param into a field, THEN
// mutates it — confirms the alias carries the value IN (read) as well as OUT
// (write). n=10 → _v := 10, then n := 11; returns n + _v = 11 + 10 = 21.
// Pre-fix: malformed IR.
BEZEL_TEST(ref_out_generic_ctor_2283, generic_class_ref_ctor_reads_and_writes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public _v: int; "
        "  public constructor(ref n: int) _v := n; n := n + 1; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var n: int := 10; "
        "    using var b := new Box<int>(ref n); "
        "    return n + b._v; "
        "  end Main; "
        "end C;"), 21);
}

// §22.3 / fix #2283: the `out` sibling on a generic ctor — "for `out` no
// getter call occurs", the callee assigns on every normal path. Pre-fix:
// malformed IR. Post-fix: 7.
BEZEL_TEST(ref_out_generic_ctor_2283, generic_class_out_ctor_param_assigns) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  private _v: int := 0; "
        "  public constructor(out n: int) n := 7; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    using var b := new Box<int>(out n); "
        "    return n; "
        "  end Main; "
        "end C;"), 7);
}

// =============================================================================
// The record-body ctor sibling (same inline-path gap, same fix). Pre-fix:
// malformed IR. Post-fix: 15.
// =============================================================================

// §22.3 / fix #2283: a `ref` param on a RECORD constructor — the record-body
// ctor inline path (buildRecordValue) had the identical paramSlots_ gap. n=10,
// ctor does n := n + 5 = 15. Pre-fix: `add ptr, i32` malformed IR.
BEZEL_TEST(ref_out_generic_ctor_2283, record_ref_ctor_param_mutates) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record R "
        "  public _v: int; "
        "  public constructor(ref n: int) n := n + 5; _v := n; end constructor; "
        "end R; "
        "public class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    var r := new R(ref x); "
        "    return x; "
        "  end Main; "
        "end C;"), 15);
}

// =============================================================================
// Controls — must keep working (no false reject). Pre- and post-fix.
// =============================================================================

// §22.3: a `ref` param on a NON-GENERIC class ctor uses the compiled-function
// path (bindParameters registers refParams_), so it worked pre-fix. Pinned so
// the inline-path fix does not regress it. n=0 → n := n + 1 = 1.
BEZEL_TEST(ref_out_generic_ctor_2283, non_generic_class_ref_ctor_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  private _v: int := 0; "
        "  public constructor(ref n: int) n := n + 1; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    using var b := new Box(ref n); "
        "    return n; "
        "  end Main; "
        "end C;"), 1);
}

// §22.3: a plain VALUE param on a generic ctor — no ref/out, no address; the
// paramSlots_ binding is correct for values. Pinned so the fix does not
// regress it. _held := 42 → 42.
BEZEL_TEST(ref_out_generic_ctor_2283, generic_class_value_param_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box<K> "
        "  public _held: int; "
        "  public constructor(item: int) _held := item; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    return b._held; "
        "  end Main; "
        "end C;"), 42);
}
