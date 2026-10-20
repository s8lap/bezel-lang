// fix #2567 (§23.4/§28.9/§4.6, v0.236): the COMPOUND-ASSIGNMENT twin
// of #2548's three-site producer-RHS release in `lowerIndexerSet`.
// `lowerIndexerCompound` (LlvmBackend_Assign.cpp:2490-2491) desugars
// `a[i] OP= rhs` to `a[i] := a[i] OP rhs`: it computes `rhs =
// lowerExpr(*as.value)` (a +1 string for the canonical `"abc" +
// i.ToString()` shape), then `newVal = emitCompoundOp(op, cur, rhs,
// ...)` (a SECOND +1 producer — emitStringConcat allocates rc=1 per
// #422), then runs the setter body with `value = newVal` (line 2488
// binds `locals_[pd] = newVal`). For a CLASS receiver, the setter (or
// inlined setter body) RETAINs its incoming `value` (§23.3 / §9),
// doubling newVal's +1 — the doubled +1 was unowned and leaked per
// call. #2548 closed the same shape for plain assignment
// (`lowerIndexerSet`'s inline-body emitProducerRelease at line 2153-
// 2154); the compound path had no analogue.
//
// Pre-fix: every iteration of `h[i] += "abc" + i.ToString()` on a
// class indexer leaks 86 byte(s) (37 byte(s) for newVal's +1, 37
// byte(s) for rhs's +1, 12 byte(s) indirect, plus the original 32+5
// indirect tail — total ~430 byte(s) for 5 iterations, all DIRECT
// leaks of bz_string headers — confirmed by probe). The leak is
// arity-independent: 1-arg and 2-arg user-defined indexers on a
// CLASS both leak on `+=`. This commit closes the gap with the
// compound-assignment twin of #2548's emitProducerRelease: after
// the setter body, gated on `recvDecl->kind == Class`, drop both
// newVal's +1 (the OP-result's doubled +1) and rhs's +1 (the rhs's
// own +1 — emitCompoundOp does NOT consume rhs, it memcpy's rhs's
// bytes into newVal and leaves rhs's +1 untouched). The record
// path's plain-store is preserved (releasing would drop the field's
// only reference — UAF), the same gate as `lowerIndexerSet`'s fix.
//
// The issue's reported repro (`Grid._cells[i*4+j] := value` with
// `public static _cells: Array<string>` and a static indexer) does
// not compile on current spec/impl: §23.5 forbids static indexers,
// and §22.12 rule 2 forbids mutable static fields of `Array<string>`
// type. The reported leak size of "37 byte(s) per iteration"
// matches this compound-assignment leak exactly (32 byte bz_string
// header + ~5 byte int-digit payload), strongly suggesting the
// issue author observed a `+=` shape and transcribed it as `:=`.
// The plain-assignment `:=` inline-setter path (the literal claim
// in the issue title) does NOT leak — #2548's three-site fix already
// covers it arity-independently (the inline-body lambda at line
// 1768-1793 fires unconditionally on the inline-body path's exit,
// only gated on `recvDecl->kind == Class`; the inline path has no
// arity-dependent branching). The plain-`:=` probes (Probes A–F)
// below remain as regression guards for #2548's still-in-place fix.
//
// Each probe loops 5× so LSan can decisively observe a 5×-amplified
// per-iteration leak if one ever lands; the exit code is `sink mod
// 256` so a passing run independently confirms the program also
// computed correctly (independent of the leak oracle).
//
// SPEC CITATION: §23.4 (user-defined indexer properties — setter
// semantics), §28.9 (runtime-managed types refcounting on producer
// transfer), §4.6 (string RC).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 9 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 9 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

// ----------------------------------------------------------------------
// Probe A — 2-arg inline-setter on a CLASS. The inline body stores
// `value` into a class field (`_items := value`), the class field
// retains, and the producer's +1 is balanced by #2548's post-body
// `emitProducerRelease()`. On HEAD this is LSan-clean (the #2567
// repro does not reproduce — see header).
// ----------------------------------------------------------------------
const char* kTwoArgInlineClassFieldLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int, j: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var sink := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      h[0, i] := \"abc\" + i.ToString(); "  // producer RHS, 2-arg inline
    "      sink := sink + h[0, i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — 2-arg inline-setter on a CLASS, body DELEGATES to a 1-arg
// Array<string> indexer set (`_cells[k] := value`). Same producer
// RHS, but the body's inner write goes through `lowerCollectionIndexerSet`
// (the inner retain path), and the outer `emitProducerRelease()`
// closes the producer's +1. On HEAD this is LSan-clean.
// ----------------------------------------------------------------------
const char* kTwoArgInlineClassArrayDelegateLoop =
    "module M; "
    "class Holder "
    "  public owned _cells: Array<string> := "
    "    [\"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\", "
    "     \"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\"]; "
    "  public property [i: int, j: int]: string "
    "    get => _cells[i * 4 + j]; "
    "    set => _cells[i * 4 + j] := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var sink := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      h[0, i] := \"abc\" + i.ToString(); "  // producer RHS, 2-arg delegation
    "      sink := sink + h[0, i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — 3-arg inline-setter on a CLASS (broader sweep: N>2).
// Same shape as Probe A with three indices. Exercises the multi-arg
// indexer-resolution path (`selectIndexer`'s nargs loop) and the
// inline-body path with N>2.
// ----------------------------------------------------------------------
const char* kThreeArgInlineClassLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int, j: int, k: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var sink := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      h[0, i, 0] := \"abc\" + i.ToString(); "  // producer RHS, 3-arg inline
    "      sink := sink + h[0, i, 0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — 1-arg inline-setter on a CLASS. REGRESSION GUARD for
// #2548's already-fixed path: must stay clean. If a future change
// over-releases on the 1-arg class inline path, this test fails.
// ----------------------------------------------------------------------
const char* kOneArgInlineClassLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var sink := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      h[i] := \"abc\" + i.ToString(); "  // producer RHS, 1-arg inline
    "      sink := sink + h[i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe E — 2-arg interface-dispatch inline-setter. Itable path on a
// 2-arg shape: regression guard for #2548's itable classBB
// emitProducerRelease branch on a 2-arg IndexExpr.
// ----------------------------------------------------------------------
const char* kTwoArgInterfaceDispatchLoop =
    "module M; "
    "interface IHolder property [i: int, j: int]: string; end IHolder; "
    "class Holder: IHolder "
    "  public _items: string := \"\"; "
    "  public property [i: int, j: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var ih: IHolder := h; "
    "    var sink := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      ih[0, i] := \"abc\" + i.ToString(); "  // producer RHS, 2-arg itable
    "      sink := sink + ih[0, i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe F — 2-arg class-vtable inline-setter. Vtable-slot dispatch
// on a 2-arg shape: regression guard for #2548's class-vtable
// `emitProducerRelease()` (unconditional on the vtable path) on a
// 2-arg IndexExpr.
// ----------------------------------------------------------------------
const char* kTwoArgClassVtableLoop =
    "module M; "
    "class Base "
    "  public _items: string := \"\"; "
    "  public virtual property [i: int, j: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Base; "
    "class Derived: Base end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var sink := 0; "
    "    var i := 0; "
    "    while i < 5 do "
    "      b[0, i] := \"abc\" + i.ToString(); "  // producer RHS, 2-arg vtable
    "      sink := sink + b[0, i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe G — 2-arg COMPOUND-ASSIGNMENT inline-setter. The
// `lowerIndexerCompound` desugars `a[i] OP= rhs` to `a[i] := a[i] OP
// rhs`: `rhs = lowerExpr(*as.value)` (a +1 string producer for
// `"abc" + i.ToString()`), then `newVal = emitCompoundOp(...)` (a
// SECOND +1 producer), then the setter body stores newVal. The
// class-field retain doubles newVal's +1 AND rhs's +1 has no owner.
// Pre-fix #2567: 86 byte(s) per iteration leaked (430 byte(s) / 5
// iterations). With the fix: 0 byte(s) leaked.
// ----------------------------------------------------------------------
const char* kTwoArgCompoundClassLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int, j: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h[0, 0] := \"abc0\"; "
    "    var sink := h[0, 0].Length; "
    "    var i := 1; "
    "    while i < 6 do "
    "      h[0, i] += \"abc\" + i.ToString(); "  // 2-arg += producer RHS
    "      sink := sink + h[0, i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe H — 1-arg COMPOUND-ASSIGNMENT inline-setter. Broader sweep
// confirming the compound-path fix is arity-independent (the same
// shape leaks on 1-arg indexers — `lowerIndexerCompound` has no
// arity-dependent branching).
// ----------------------------------------------------------------------
const char* kOneArgCompoundClassLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h[0] := \"abc0\"; "
    "    var sink := h[0].Length; "
    "    var i := 1; "
    "    while i < 6 do "
    "      h[i] += \"abc\" + i.ToString(); "  // 1-arg += producer RHS
    "      sink := sink + h[i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

// ----------------------------------------------------------------------
// Probe I — 1-arg COMPOUND-ASSIGNMENT with a NAMED LOCAL RHS (a
// borrow, NOT a producer). Regression guard: the fix MUST NOT
// release a borrow-rhs (the caller still owns the +1). With the
// gate `isPlusOneStringProducer(as.value)` this correctly skips
// the rhs release (IdentifierExpr("t") returns false); the
// setter's retain on newVal closes the producer side.
// ----------------------------------------------------------------------
const char* kOneArgCompoundNamedLocalClassLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h[0] := \"abc0\"; "
    "    var t := \"abc\"; "
    "    var sink := h[0].Length + t.Length; "
    "    var i := 1; "
    "    while i < 6 do "
    "      h[i] += t; "  // 1-arg += with named local RHS (borrow, not producer)
    "      sink := sink + h[i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end; "
    "end C;";

}  // namespace

// =====================================================================
// #2567 — Probe A: 2-arg inline-setter on a CLASS, simple field store.
// Cardinal case: this passes on HEAD (the #2567 leak claim does not
// reproduce — see header for the investigation). Regression guard for
// #2548's post-body emitProducerRelease on the 2-arg inline-body path.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           two_arg_inline_class_field_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kTwoArgInlineClassFieldLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe B: 2-arg inline-setter on a CLASS, body delegates to
// 1-arg Array<string> indexer set. Regression guard for the
// `lowerCollectionIndexerSet` inner-call path when invoked from
// inside the inline-setter body (where `valueExpr` is an
// `IdentifierExpr("value")`).
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           two_arg_inline_class_array_delegate_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kTwoArgInlineClassArrayDelegateLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe C: 3-arg inline-setter on a CLASS. Broader N>2 sweep.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           three_arg_inline_class_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kThreeArgInlineClassLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe D: 1-arg inline-setter on a CLASS. REGRESSION GUARD
// for #2548's already-fixed single-arg inline-body path.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           one_arg_inline_class_regression_clean_spec_23_4_28_9) {
    JitResult r = runAotChecked(kOneArgInlineClassLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe E: 2-arg interface-dispatch inline-setter. Itable
// path on a 2-arg shape: regression guard for #2548's classBB.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           two_arg_interface_dispatch_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kTwoArgInterfaceDispatchLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe F: 2-arg class-vtable inline-setter. Vtable-slot
// dispatch on a 2-arg shape: regression guard for #2548's
// class-vtable emitProducerRelease.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           two_arg_class_vtable_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kTwoArgClassVtableLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe G: 2-arg COMPOUND-ASSIGNMENT (`+=`) inline-setter on
// a CLASS. The cardinal case for the fix: pre-fix leaks 86 byte(s)
// per iteration (430 byte(s) / 5 iterations). With the fix: 0
// byte(s) leaked.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           two_arg_compound_class_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kTwoArgCompoundClassLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe H: 1-arg COMPOUND-ASSIGNMENT (`+=`) inline-setter on
// a CLASS. Broader sweep — the fix is arity-independent.
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           one_arg_compound_class_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kOneArgCompoundClassLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2567 — Probe I: 1-arg COMPOUND-ASSIGNMENT with NAMED LOCAL RHS
// (a borrow, not a producer). Regression guard: the fix's gate
// `isPlusOneStringProducer(as.value)` correctly returns false for
// an `IdentifierExpr("t")` and skips the rhs release — releasing
// would drop the local's +1 (UAF).
// =====================================================================
BEZEL_TEST(codegen_multi_arg_indexer_inline_setter_2567,
           one_arg_compound_named_local_class_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kOneArgCompoundNamedLocalClassLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
