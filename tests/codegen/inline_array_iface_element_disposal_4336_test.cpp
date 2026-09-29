// narrow #4336 (broaden coverage): OWNING inline-array-of-interface element
// disposal is engaged on every storage shape #4317's fix touched (per
// §15.1.1 / §15.3 / §28.7, v0.300).
//
// The narrower regression for the single-index drop-on-overwrite half
// already lives at `tests/codegen/owning_inline_array_index_assign_drop_on_overwrite_4336_test.cpp`
// (commit `2fca69a7` in `origin/main`). The `#4317` family touched FOUR
// inline-array STORE call sites (per its issue body) — the IndexExpr-arm
// plain-array-assignment, the whole-array collection-literal REASSIGNMENT
// arm, the ReturnStmt collection-literal arm, AND the local-decl init
// collection-literal arm. #4336 narrowed the disposal half of the same
// four; the existing regression covers (1) — this file covers the other
// three: local-decl init, whole-array reassignment, and ReturnStmt —
// all under the OWNING shape (`[N owned]IFoo`), the spec-conformant
// disposition (`registerInlineOwnedArrayDisposal` for the local-decl init
// path; `lowerOwningInlineArrayReassign` (#1385 / #3263) for the whole-
// array reassign path; the ReturnStmt path is reached through the same
// local-decl init registration at the caller's `var outer := Helper.Wrap()`
// site).
//
// Each test amplifies to 100 iterations per `lsan-one-shot-leak-tests-pass-vacuously.md`
// (one-shot LSan reports would conservatively mark the still-on-stack
// final-slot reachable and pass BOTH pre- and post-fix — measured on the
// issue's repro shape: 1× leaks 0 of 2; 100× leaks 99 of 100). The
// post-fix PASS-WITH expectation matches the existing test's
// `r.ran = true` (the harness's LSan gate).
//
// NON-OWNING SCOPE: per spec §15.1 line 2868, "the elements stored in
// the array are *borrows* — the initializer expressions must be borrows
// of class instances already owned elsewhere", and the issue's literal
// repro `var outer: [1]IHolder := [new Widget("init")];` (no `owned`)
// contradicts this. Sema does NOT currently enforce the borrow-only
// contract (the program compiles and runs, leaking the freshly-allocated
// Widget — measured: 4800 B / 200 + 4776 B / 199 on HEAD). That leak is
// filed separately as #4351 (spec/sema ruling question, out of scope for
// #4336). This file deliberately tests ONLY the OWNING `[N owned]IFoo`
// shape so it remains PASS-WITH on HEAD; the borrow-shape leak paths
// are not regressed by adding them here (a `#4351` lane would file a
// sema-side fix or change codegen policy, both of which are owner-decision
// territory per `lane-rules-r7` §9 / `crashed-run-wip-must-be-rederived`).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ASan toolchain skip-guard (mirrors #2548/#4317/#4336's harness helper).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// =============================================================================
// #4336 — OWNING local-decl-init of an inline array of interface elements
// disposes each non-null slot at scope end (the §15.1.1 / §15.3 contract,
// wired by `registerInlineOwnedArrayDisposal` at the local-decl site). This
// is the "fourth" call site of the #4317 family (the local-decl init
// collection-literal arm at `LlvmBackend_Setup.cpp:14585-`, which stores
// each element via GEP after boxing any record-typed element via
// `boxForTarget` per fix #4335).
//
// Loop-amplified to 100 iterations per LSan ratchet (one-shot would mark
// the still-on-stack final slot reachable on BOTH sides); LSan-clean iff
// the scope-end auto-dispose pass fires for the owning flag — gated on
// `arrRt->arrayOwned = true` in `registerInlineOwnedArrayDisposal`. A
// FAIL-WITHOUT case here would be a regression to a pre-#4317 compiler
// where the local-decl init's per-element store bypassed any disposal
// registration — measured pre-#4317: 2 leaks / 2 objects per iteration.
// =============================================================================
const char* kLocalDeclInitLoop =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "  public method GetV(): string => _s; "
    "end Widget; "
    "class C "
    "  public static method Once(): int "
    "    var outer: [2 owned]IHolder := [new Widget(\"init\"), new Widget(\"two\")]; "
    "    return outer[0].GetV().Length + outer[1].GetV().Length; "
    "  end Once; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      total := total + C.Once(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";
// Sink: init=4, two=3; per iter 4+3=7; ×100 = 700 mod 256 = 700-2*256 = 700-512 = 188.
BEZEL_TEST(inline_array_iface_element_disposal_4336,
           owning_local_decl_init_scope_end_disposal_spec_15_1_1_15_3) {
    JitResult r = runAotChecked(kLocalDeclInitLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 188);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #4336 — OWNING whole-array collection-literal REASSIGNMENT disposes every
// displaced live slot before the new-value store (§15.3's whole-value
// reassignment rule — `lowerOwningInlineArrayReassign`, #1385 / #3263).
// This is the second of #4317's four call sites (`LlvmBackend_Stmts.cpp:~898`
// and `~1298` for two different whole-array contexts).
//
// The pre-fix compiler stores the new aggregate over the old one without
// disposing the old slots — measured 2 leaks per iteration (200 B in the
// issue's amp shape). The post-fix compiler inserts a per-slot dispose
// pass on every non-null displaced slot. Loop-amplified to 100 iterations
// per LSan ratchet.
// =============================================================================
const char* kWholeArrayReassignLoop =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "  public method GetV(): string => _s; "
    "end Widget; "
    "class C "
    "  public static method Once(): int "
    "    var outer: [2 owned]IHolder := [new Widget(\"init\"), new Widget(\"two\")]; "
    "    outer := [new Widget(\"hello\"), new Widget(\"world\")]; "
    "    return outer[0].GetV().Length + outer[1].GetV().Length; "
    "  end Once; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      total := total + C.Once(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";
// Sink: hello=5, world=5; per iter 5+5=10; ×100 = 1000 mod 256 = 232.
BEZEL_TEST(inline_array_iface_element_disposal_4336,
           owning_whole_array_reassign_disposes_displaced_spec_15_3) {
    JitResult r = runAotChecked(kWholeArrayReassignLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 232);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #4336 — OWNING ReturnStmt of a collection literal: the CALLEE has nothing
// to register (the array is value-typed, returned through the §15.1 value
// convention), but the CALLER's `var outer := Helper.Wrap()` local-decl
// init must engage the same `registerInlineOwnedArrayDisposal` registration
// that a literal initializer would — this is the third of #4317's four
// call sites (the ReturnStmt collection-literal arm at the same Stmts.cpp
// site as the IndexExpr arm).
//
// Loop-amplified to 100 iterations; LSan-clean iff the callee's
// freshly-allocated Widgets are disposed by the caller's scope-end pass.
// =============================================================================
const char* kReturnStmtLoop =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "  public method GetV(): string => _s; "
    "end Widget; "
    "class Helper "
    "  public static method Wrap(): [2 owned]IHolder "
    "    return [new Widget(\"hi\"), new Widget(\"world\")]; "
    "  end Wrap; "
    "end Helper; "
    "class C "
    "  public static method Once(): int "
    "    var outer := Helper.Wrap(); "
    "    return outer[0].GetV().Length + outer[1].GetV().Length; "
    "  end Once; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      total := total + C.Once(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";
// Sink: hi=2, world=5; per iter 2+5=7; ×100 = 700 mod 256 = 188.
BEZEL_TEST(inline_array_iface_element_disposal_4336,
           owning_return_stmt_caller_disposes_callee_allocations_spec_15_1_1) {
    JitResult r = runAotChecked(kReturnStmtLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 188);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #4336 — POSITIVE CONTROL: an `[N]int` inline array (value-typed elements,
// no heap allocation) is unaffected by the disposal paths. Confirms the
// fix did not regress the trivial scalar case.
// =============================================================================
const char* kIntArrayLoop =
    "module M; "
    "class C "
    "  public static method Once(): int "
    "    var arr: [3]int := [1, 2, 3]; "
    "    return arr[0] + arr[1] + arr[2]; "
    "  end Once; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      total := total + C.Once(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";
// Sink: per iter 1+2+3=6; ×100 = 600 mod 256 = 600-2*256 = 600-512 = 88.
BEZEL_TEST(inline_array_iface_element_disposal_4336,
           int_array_unaffected_by_disposal_paths_spec_15_1) {
    JitResult r = runAotChecked(kIntArrayLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 88);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// #4336 — POSITIVE CONTROL: an `[N owned]IFoo` local-decl init engages
// scope-end disposal for the freshly-allocated Widgets (the brief's case 5
// in the owning-positive form). Pairs with the EXISTING single-index
// overwrite test to cover the full owning-flag-true surface that the
// spec requires disposal for. A FAIL here would be a regression to the
// pre-#4317 / pre-#4336 compiler where local-decl init's per-element
// store bypassed any disposal registration — measured pre-#4317:
// 2 leaks / 2 objects per iteration (the freshly-allocated Widgets).
// =============================================================================
const char* kOwnedLocalDeclLoop =
    "module M; "
    "interface IHolder "
    "  method GetV(): string; "
    "end IHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) "
    "    _s := s; "
    "  end constructor; "
    "  public method GetV(): string => _s; "
    "end Widget; "
    "class C "
    "  public static method Once(): int "
    "    var arr: [3 owned]IHolder := [new Widget(\"hi\"), new Widget(\"world\"), new Widget(\"!\")]; "
    "    return arr[0].GetV().Length + arr[1].GetV().Length + arr[2].GetV().Length; "
    "  end Once; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      total := total + C.Once(); "
    "      i := i + 1; "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";
// Sink: hi=2, world=5, !=1; per iter 2+5+1=8; ×100 = 800 mod 256 = 800-3*256 = 800-768 = 32.
BEZEL_TEST(inline_array_iface_element_disposal_4336,
           owned_interface_array_local_decl_init_disposes_spec_15_1_1) {
    JitResult r = runAotChecked(kOwnedLocalDeclLoop);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 32);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}