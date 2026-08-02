// Spec-derived tests for issue #2318 — `recv?.M(ref x)` drops the `ref`/`out`
// modifier entirely: `Emitter::lowerNullCondCall` (the `?.` call form, §7.5)
// built its argument list with a bare loop that lowered EVERY argument as a
// plain VALUE, ignoring the parameter's `ref`/`out` modifier (§22.3). The
// `.` form (`lowerCall`) instead routes through `buildOrderedArgs`, whose
// CG50 branch passes the ARGUMENT'S ADDRESS for a `ref`/`out` parameter — so
// `recv?.M(ref x)` and `recv.M(ref x)` had DIFFERENT argument-lowering paths
// for the identical §22.3 modifier.
//
// As filed, this failed the IR verifier for a value-typed `ref` parameter
// (`ref a: int` — the caller's loaded `int` VALUE reached a callee expecting
// a `ptr`, an LLVM type mismatch the verifier catches):
//
//   Call parameter type does not match function signature!
//     %10 = load i32, ptr %0, align 4
//    ptr  call void @"bz$Helper$Bump"(ptr %8, i32 %10)
//
// But for a POINTER-REPRESENTED `ref` parameter (a class, interface, or
// `string` — whose VALUE is already an LLVM `ptr`, the same type the callee
// expects for the address), the verifier sees two matching `ptr` types and
// passes — and the callee then dereferences the caller's STRING VALUE as if
// it were the caller's STACK ADDRESS, corrupting memory. Verified live during
// this fix: `nullcond_ptr_ref_param_silent_miscompile_is_fixed` below reran
// the PRE-fix binary under ASan and observed a SEGV (`AddressSanitizer:
// DEADLYSIGNAL ... WRITE ... in bz$Helper$Set`) — the "silent miscompile
// risk" the issue flagged, confirmed, not merely feared.
//
// The fix routes `lowerNullCondCall`'s argument list through the SAME
// `buildOrderedArgs` helper `lowerCall` already uses (LlvmBackend_Calls2.cpp),
// so a `ref`/`out` argument gets CG50's address-of shaping and — per #1302,
// landed the day before this issue was filed (§22.3 "accessor targets
// bracket") — an indexer-slot/property `ref`/`out` argument gets #1302's
// copy-in/copy-out bracket instead of a raw (and for those locations,
// nonexistent) address. Because the whole argument-lowering block already
// ran, before and after this fix, strictly inside the `?.` call's "receiver
// is non-null" basic block, a null receiver skips argument evaluation
// entirely — matching §7.5's "if any segment of a chain is null, the
// remainder is not evaluated" — with no additional special-casing: the
// callee's `ref` alias is left untouched, and an indexer/property target's
// getter and setter never run. This composes with #1302 rather than
// bypassing it (bracket tests below) and does not touch the `.` (non-`?.`)
// call path (the `ref_out`/`1302` filter tests above these are unaffected).
//
// DECISION ON `out` THROUGH `?.` (CLAUDE.md rule 4 — recorded, not silent):
// §22.3's `out` contract ("the callee must assign every `out` parameter on
// every normal-completion (return) path") and §22.4's per-path
// definite-assignment treatment of an inline `out var` recipient ("assigned
// only on control-flow paths where the introducing call was actually
// evaluated") are both stated for a call that RAN. Neither clause addresses
// a null-conditional call's null-receiver path, on which the callee never
// runs at all — a third outcome beyond "returned normally" / "threw".
// DefiniteAssignment.cpp already gives `and`/`or` short-circuiting this
// per-path treatment (evalCond's branch-local `assigned_` sets) but had NO
// equivalent for `?.`: a `NullCondMemberExpr`-callee `CallExpr`'s args were
// walked exactly like an ordinary `.` call's (visitCallArg unconditionally
// marked an `out` recipient "assigned" on the straight-line path — see
// DefiniteAssignment.cpp ~line 1344). Admitting `out` through `?.` at the
// time would therefore have let a non-nullable-reference-typed inline
// `out var` recipient read as "definitely assigned" on a path where it was
// never written — exactly the read §22.4 exists to forbid. Rather than
// invent an uncited narrower rule (e.g. "admit only a pre-declared,
// already-assigned recipient" — the one sub-case that happens to be safe,
// verified in an early probe: `n := 10; hh?.Set(out n);` with `hh = null`
// left `n == 10`, no crash) this fix REJECTED the whole `out`/`out
// owned`-through-`?.` shape uniformly, with a diagnostic citing §22.3/§7.5
// (ExprTypecheck_Calls.cpp, the `NullCondMemberExpr` block), and filed
// #2345 to track lifting this once `?.` gets the same per-path DA modeling
// `and`/`or` already have. `ref` carries no such hazard — it requires the
// argument already assigned BEFORE the call, so a skipped call leaves an
// already-valid value; DA does not newly assert anything about it. #2319
// (accessor-mediated `ref`/`out` on CONSTRUCTOR calls — ctor sites have no
// post-call flush point) is a distinct AST shape (`NewExpr`, not this
// `CallExpr`/`NullCondMemberExpr` pair) and is not touched here.
//
// #2345 UPDATE (this file, v0.235): DefiniteAssignment.cpp now has that
// per-path modeling (evalNullCondCallArgs, mirroring evalCond's and/or
// split — see DefiniteAssignment.cpp), and ExprTypecheck_Calls.cpp's
// NullCondMemberExpr block narrows the ban to the two shapes NOT yet
// verified safe: any `out owned` callee parameter (every caller spelling —
// the ownership pass's own treatment of a conditionally-run `out owned`
// assignment through `?.` is unaudited) and an accessor-mediated
// pre-declared target (`out arr[i]`, `out obj.Prop` — #1302's bracket has
// only been verified composed with `?.` for `ref`, never `out`). Both
// remain rejected below (filed #2393). A pre-declared bare-identifier
// local/parameter recipient and the inline `out var x` form are now
// admitted, gated on DA: a non-nullable-reference inline recipient is
// definitely-assigned only immediately within the call's own live
// continuation, so ANY later read is a DA "read of unassigned local" error
// — regardless of the receiver's actual runtime nullness, since DA does not
// perform cross-statement null-flow narrowing of the receiver — while a
// NULLABLE inline recipient (never DA-tracked, §22.4's
// `inlineOutVarNeedsDA`) is always definitely-assigned and usable behind an
// ordinary nullable-narrowing guard. See
// nullcond_out_da_2345_test.cpp for the DA-focused coverage (chained
// access, the null-forgiving `!` non-interaction, and the accepted/
// rejected shape matrix); the two tests immediately below are updated in
// place because their expected OUTCOME changed, per this task's explicit
// instruction that a lifted rejection's test must change.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `Bump(ref a: int)` adds 5; an INSTANCE method (unlike
// ref_out_accessor_writeback_1302_test.cpp's static kBump) because every
// null-conditional test below calls it through a nullable INSTANCE receiver
// (`hh?.Bump(...)`, matching the issue's own repro, whose `Helper.Bump` is
// likewise declared as an instance method) — a static method reached via
// `?.` is a different, unrelated shape (`isStaticMethod(ms)` makes
// `lowerNullCondCall` bail before emitting anything at all, so the call
// silently no-ops instead of exercising this fix either way).
const char* kBump =
    "  public method Bump(ref a: int) a := a + 5; end Bump; ";
}  // namespace

// =============================================================================
// The issue's own repro, and its "receiver is a local/field" siblings —
// `ref` targeting a TRUE-ALIAS location (§22.3 "ref local and field targets
// alias").
// =============================================================================

// The issue's literal repro (#2318): `hh?.Bump(ref n)` on a NON-null
// receiver. Pre-fix: IR verification failure ("Call parameter type does not
// match function signature!"). Post-fix: the write-back is visible, 10 + 5.
BEZEL_TEST(nullcond_ref_out_2318, ref_to_local_writes_back_on_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Helper ") + kBump + "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    var n := 10; "
        "    hh?.Bump(ref n); "
        "    return n; "
        "  end Main; end C;"), 15);
}

// The receiver's OTHER branch: `hh` is null, so §7.5's short-circuit means
// `Bump` never runs at all. Pre-fix this ALSO hit IR verification failure —
// LLVM verifies the whole function body statically, so the malformed IR on
// the (unreached-at-runtime) call branch still failed to compile. Post-fix:
// `n` is left untouched by an alias that was never written through, and the
// program compiles and runs cleanly (no crash, no diagnostic).
BEZEL_TEST(nullcond_ref_out_2318, ref_to_local_untouched_on_null_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Helper ") + kBump + "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var hh: Helper? := null; "
        "    var n := 10; "
        "    hh?.Bump(ref n); "
        "    return n; "
        "  end Main; end C;"), 10);
}

// The field sibling of the local case — `ref` to a writable field is a true
// alias too (§22.3).
BEZEL_TEST(nullcond_ref_out_2318, ref_to_field_writes_back_on_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Helper ") + kBump + "end Helper; "
        "class C public N: int := 10; "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    using var c := new C(); "
        "    hh?.Bump(ref c.N); "
        "    return c.N; "
        "  end Main; end C;"), 15);
}

// =============================================================================
// Composing with #1302: an indexer-slot `ref` argument through `?.` needs
// BOTH the null-guard (this fix) AND the copy-in/copy-out write-back (#1302)
// — the exact composition the task calls out.
// =============================================================================

// Non-null receiver: the getter runs once, `Bump` runs, the setter runs once
// on normal return (#1302's bracket) — 10 + 5, exactly as the plain `.` form
// (ref_out_accessor_writeback_1302_test.cpp's own
// ref_to_heap_array_slot_writes_back) already proves for `.`.
BEZEL_TEST(nullcond_ref_out_2318, ref_to_indexer_slot_writes_back_on_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Helper ") + kBump + "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    hh?.Bump(ref arr[0]); "
        "    return arr[0]; "
        "  end Main; end C;"), 15);
}

// Null receiver: neither the getter NOR the setter may run — the setter in
// particular must not fire on a call that never happened (self-pass
// requirement (b)). Independently, the INDEX EXPRESSION itself must not be
// evaluated either: §22.3's evaluate-once discipline ("the receiver and every
// index expression are evaluated exactly once ... for `ref`, the getter is
// called once") and §7.5's short-circuit ("the remainder is not evaluated")
// both have a claim on this index expression, and they agree on the answer
// for OPPOSITE reasons that only coincide because the argument-lowering that
// would evaluate the index sits entirely inside the call's "receiver
// non-null" basic block: §7.5 wins procedurally (the index expression is
// part of "the remainder" a null receiver never reaches), and because it
// wins, #1302's evaluate-once accounting never starts, so there is no
// once-vs-zero conflict to resolve. `NextIdx()` increments a static counter
// and returns the index; the result packs "was NextIdx called" and "was the
// slot written" into one integer so both are pinned by one assertion: 0 calls
// x 100 + the original element (10) = 10 (compare against the non-null
// sibling test below, which packs 1 x 100 + 15 = 115).
BEZEL_TEST(nullcond_ref_out_2318,
           ref_to_indexer_slot_index_not_evaluated_and_setter_not_called_on_null_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Helper ") + kBump + "end Helper; "
        "class C "
        "  public static counter: int := 0; "
        "  public static method NextIdx(): int "
        "    C.counter := C.counter + 1; return 0; "
        "  end NextIdx; "
        "  public static method Main(): int "
        "    var hh: Helper? := null; "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    hh?.Bump(ref arr[C.NextIdx()]); "
        "    return C.counter * 100 + arr[0]; "
        "  end Main; end C;"), 10);
}

// The non-null counterpart of the test above: the index expression IS
// evaluated, exactly ONCE (#1302's evaluate-once discipline), and the slot IS
// written — 1 x 100 + (10 + 5) = 115.
BEZEL_TEST(nullcond_ref_out_2318,
           ref_to_indexer_slot_index_evaluated_once_on_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Helper ") + kBump + "end Helper; "
        "class C "
        "  public static counter: int := 0; "
        "  public static method NextIdx(): int "
        "    C.counter := C.counter + 1; return 0; "
        "  end NextIdx; "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    hh?.Bump(ref arr[C.NextIdx()]); "
        "    return C.counter * 100 + arr[0]; "
        "  end Main; end C;"), 115);
}

// =============================================================================
// The silent-miscompile risk the issue named explicitly: a POINTER-
// REPRESENTED `ref` parameter (here `string`) passes the IR verifier either
// way (both the caller's loaded value and the expected address are LLVM
// `ptr`), so this shape does NOT fail to compile pre-fix — it compiles clean
// and then corrupts memory at runtime. Verified during this fix: rerunning
// the PRE-fix binary for this exact program under ASan produced
// `AddressSanitizer: DEADLYSIGNAL ... SEGV ... WRITE ... in bz$Helper$Set`
// (the callee treating the string's own pointer VALUE as a stack ADDRESS and
// storing through it). Post-fix the callee receives the local's true address
// and the write-back is the ordinary string value it should be.
// =============================================================================
BEZEL_TEST(nullcond_ref_out_2318, ref_to_string_local_writes_back_no_miscompile) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Helper "
        "  public method Set(ref a: string) a := \"changed\"; end Set; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    var s := \"orig\"; "
        "    hh?.Set(ref s); "
        "    return (if s = \"changed\" then 1 else 0); "
        "  end Main; end C;"), 1);
}

// =============================================================================
// `out` through `?.` — narrowed by #2345 (see the file header's updated
// decision). The inline `out var` form (v0.235: admitted at sema, but a
// non-nullable recipient is now caught by DefiniteAssignment.cpp's own
// per-path split whenever it is READ afterward) and the pre-declared-local
// form (now admitted outright — runtime-safe and DA-transparent, since the
// recipient was already assigned before the call) diverge in outcome; the
// `out owned` shape remains rejected. Reject-test diagnostic text is
// asserted per the task's requirement.
// =============================================================================

// The inline `out var` form, READ afterward: this is the shape with the DA
// hazard (a non-nullable-reference-typed recipient). v0.235: no longer
// blocked by ExprTypecheck_Calls.cpp's blanket ban — it now reaches
// DefiniteAssignment.cpp, whose new per-path split (evalNullCondCallArgs)
// marks `b` assigned only within the call's own non-null continuation, so
// the read at `return b.V;` is DA's ordinary "read of unassigned local"
// diagnostic, not the old §22.3/§7.5 sema message. This is deliberately
// unconditional on the receiver's actual runtime nullness (`hh` is null
// here, but the SAME rejection fires even with a non-null `hh` — see
// nullcond_out_da_2345_test.cpp's
// out_var_read_afterwards_rejected_even_when_receiver_provably_nonnull —
// because DA does not perform cross-statement null-flow narrowing of the
// receiver; only the per-path split WITHIN the `?.` call's own args gets the
// and/or-style precision).
BEZEL_TEST(nullcond_ref_out_2318,
           out_var_through_nullcond_call_used_afterwards_is_rejected_by_da) {
    auto r = runAot(
        "module M; class Box public V: int := 42; end Box; "
        "class Helper "
        "  public method Make(out a: Box) a := new Box(); end Make; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    var hh: Helper? := null; "
        "    hh?.Make(out var b); "
        "    return b.V; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool citedUnassigned = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("read of unassigned local `b`") !=
            std::string::npos)
            citedUnassigned = true;
    }
    BEZEL_EXPECT_TRUE(citedUnassigned);
}

// The pre-declared-local form: this sub-case is DA-transparent (the
// recipient was already in `assigned_` before the call, so DA's new
// intersection — {n} on both the call-ran and call-skipped continuations —
// leaves it assigned unconditionally afterward) AND runtime-safe
// (#2318's own probe: `n := 10; hh?.Set(out n);` with `hh = null` left
// `n == 10`, no crash) — so #2345 admits it outright. Null-receiver case
// (companion non-null case immediately below).
BEZEL_TEST(nullcond_ref_out_2318,
           out_predeclared_local_through_nullcond_call_is_accepted_null_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Helper "
        "  public method Set(out a: int) a := 99; end Set; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    var hh: Helper? := null; "
        "    var n := 10; "
        "    hh?.Set(out n); "
        "    return n; "
        "  end Main; end C;"), 10);
}

// Same shape, non-null receiver: the call runs and `n` is overwritten.
BEZEL_TEST(nullcond_ref_out_2318,
           out_predeclared_local_through_nullcond_call_is_accepted_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Helper "
        "  public method Set(out a: int) a := 99; end Set; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    var n := 10; "
        "    hh?.Set(out n); "
        "    return n; "
        "  end Main; end C;"), 99);
}

// `out owned` through `?.` remains rejected: the OWNERSHIP pass's own
// treatment of a conditionally-run `out owned` assignment through `?.`
// is unaudited by #2345 (filed #2393) — admitting it risks reintroducing
// #2318's hazard in a different pass. Same §22.3/§7.5-citing diagnostic
// family, now naming the specific reason.
BEZEL_TEST(nullcond_ref_out_2318, out_owned_through_nullcond_call_is_rejected) {
    auto r = runAot(
        "module M; class Cell public N: int; "
        "  public constructor(n: int) this.N := n; end constructor; end Cell; "
        "class Helper "
        "  public method Make(out owned c: Cell) c := new Cell(5); end Make; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    hh?.Make(out using var c); "
        "    return c.N; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited22_3 = false, cited7_5 = false, citedOwned = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§22.3") != std::string::npos) cited22_3 = true;
        if (d.message.find("§7.5") != std::string::npos) cited7_5 = true;
        if (d.message.find("out owned") != std::string::npos)
            citedOwned = true;
    }
    BEZEL_EXPECT_TRUE(cited22_3);
    BEZEL_EXPECT_TRUE(cited7_5);
    BEZEL_EXPECT_TRUE(citedOwned);
}

// =============================================================================
// Regression guard: an ordinary (non-`?.`) `ref`/`out` call is untouched by
// this fix — this fix only changes `lowerNullCondCall`'s argument-building,
// not `lowerCall`'s. (The full ref_out_accessor_writeback_1302_test.cpp suite
// is the authoritative version of this guard; this is a single local smoke
// test kept in this file so it self-checks without cross-file coupling.)
// =============================================================================
BEZEL_TEST(nullcond_ref_out_2318, plain_dot_call_ref_to_local_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump(ref a: int) a := a + 5; end Bump; "
        "  public static method Main(): int "
        "    var n := 10; "
        "    C.Bump(ref n); "
        "    return n; "
        "  end Main; end C;"), 15);
}
