// Spec-derived tests for issue #2345 — `DefiniteAssignment` gains per-path
// short-circuit modeling for the null-conditional operator `?.`, mirroring
// the modeling it already has for `and`/`or` (§22.4's `evalCond`).
//
// BACKGROUND: #2318 found that `recv?.M(..., out x, ...)` dropped the
// `ref`/`out` modifier in codegen (fixed there) and separately discovered
// that DefiniteAssignment.cpp had NO per-path treatment for `?.`'s
// short-circuit: a `NullCondMemberExpr`-callee `CallExpr`'s arguments were
// walked exactly like an ordinary `.` call's (visitCallArg unconditionally
// marking an `out` recipient "assigned" on the straight-line path,
// DefiniteAssignment.cpp ~line 1344 at the time). Left unfixed, a
// non-nullable-reference-typed inline `out var` recipient could read as
// "definitely assigned" on the null-receiver path where it was never
// written — the exact violation §22.4 exists to forbid for `and`/`or`
// ("[an inline `out var` is] assigned only on control-flow paths where the
// introducing call was actually evaluated"). #2318 therefore rejected the
// whole `out`/`out owned`-through-`?.` shape uniformly and filed #2345 to
// supply the missing modeling.
//
// THE FIX (DefiniteAssignment.cpp): `evalNullCondCallArgs` mirrors
// `evalCond`'s `and`/`or` split — see the `evalCond`/`evalNullCondCallArgs`
// pair in that file. `visitExpr`'s `CallExpr` handling now recognizes a
// `NullCondMemberExpr` callee and routes it through
// `evalNullCondCallArgs` instead of the generic (unconditional)
// walk. Per §7.5 ("if any segment of a chain is null, the remainder is not
// evaluated"), the receiver is evaluated unconditionally but the call's
// ARGUMENTS — and any `out` assignment they produce — are visited only
// within the "receiver non-null" continuation; the two continuations are
// intersected for ordinary (non-short-circuit-composed) use, exactly as
// `evalCond`'s `and`/`or` merge already does. §22.4's own "assigned only on
// control-flow paths where the introducing call was actually evaluated"
// clause settles the DESIGN QUESTION directly: on the null-receiver path
// the recipient is simply NOT assigned (a definite-assignment fact), not a
// shape that must stay categorically rejected — precisely the reading this
// fix implements. §22.3's `out` contract statement ("normal-completion
// (return) path") is unaffected: a skipped call is neither a
// normal-completion nor an exception exit, and DA's treatment of it exactly
// parallels how `and`'s unevaluated right operand is treated today.
//
// SCOPE (ExprTypecheck_Calls.cpp's NullCondMemberExpr block, narrowed):
// admitted — a pre-declared bare-identifier local/parameter recipient, and
// the inline `out var x` declaration form (needed a companion codegen fix:
// `lowerNullCondCall` now calls `ensureOutVarSlots` before its
// receiver-null branch, exactly as `lowerCall` already does, so a fresh
// `out var x` local has a registered slot for `buildOrderedArgs`'s CG50
// branch to find). Still rejected (filed #2393, not audited by this fix):
// any `out owned` callee parameter in any caller spelling (the OWNERSHIP
// pass's own conditionally-run-assignment treatment through `?.` is
// unverified) and an accessor-mediated pre-declared target (`out arr[i]`,
// `out obj.Prop` — #1302's bracket has only been verified with `ref`
// through `?.`, never `out`).
//
// This file covers: the accepted/rejected shape matrix beyond
// nullcond_ref_out_2318_test.cpp's two updated cases (an unused inline
// recipient; a NULLABLE inline recipient, narrowed and read — the "null-
// guarded branch" acceptance case; an accessor-target and a field-target
// pre-declared recipient, both still rejected); a three-way nullness matrix
// for a CHAINED null-conditional receiver (`a?.B?.M(out n)`, multiple
// short-circuit points); confirmation that the null-forgiving `!` operator
// does not route through this machinery at all (a `hh!.M(...)` callee is a
// plain `MemberAccessExpr` on a `PostfixBangExpr`, never a
// `NullCondMemberExpr`); and a control check that `and`/`or`'s own
// short-circuit `out var` modeling (§22.4's own worked example) is
// untouched by this change.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// =============================================================================
// An inline `out var` recipient that is NEVER read afterward compiles and
// runs cleanly on both receiver branches — proving the companion codegen
// fix (ensureOutVarSlots in lowerNullCondCall) actually works end to end,
// not merely that DA declines to complain about an absent read.
// =============================================================================

BEZEL_TEST(nullcond_out_da_2345, inline_out_var_unread_compiles_null_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box public V: int := 42; end Box; "
        "class Helper "
        "  public method Make(out a: Box) a := new Box(); end Make; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    var hh: Helper? := null; "
        "    hh?.Make(out var b); "
        "    return 7; "
        "  end Main; end C;"), 7);
}

BEZEL_TEST(nullcond_out_da_2345,
           inline_out_var_unread_compiles_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box public V: int := 42; end Box; "
        "class Helper "
        "  public method Make(out a: Box) a := new Box(); end Make; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    hh?.Make(out var b); "
        "    return 7; "
        "  end Main; end C;"), 7);
}

// The DA rejection is unconditional on the receiver's actual runtime
// nullness — the SAME diagnostic fires even when `hh` is provably non-null
// (a `using var` assigned directly, never reassigned), because DA does not
// perform cross-statement null-flow narrowing of the receiver; only the
// per-path split WITHIN the `?.` call's own evaluation gets and/or-style
// precision (mirroring exactly how `and`'s own precision is scoped to its
// own two operands, not to facts a caller might separately know about a
// sub-expression). This is the "narrowest sound reading" #2345's commit
// records.
BEZEL_TEST(nullcond_out_da_2345,
           out_var_read_afterwards_rejected_even_when_receiver_provably_nonnull) {
    auto r = runAot(
        "module M; class Box public V: int := 42; end Box; "
        "class Helper "
        "  public method Make(out a: Box) a := new Box(); end Make; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    hh?.Make(out var b); "
        "    return b.V; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool citedUnassigned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("read of unassigned local `b`") !=
            std::string::npos)
            citedUnassigned = true;
    BEZEL_EXPECT_TRUE(citedUnassigned);
}

// =============================================================================
// "Null-guarded branch, accepted": a NULLABLE inline `out var` recipient is
// never DA-tracked at all (§22.4's `inlineOutVarNeedsDA` — only a
// non-nullable-reference recipient carries the hazard; a nullable one is
// default-initialized to `null` per §7.9 and readable everywhere in scope).
// Reading it behind an ordinary nullable-narrowing guard (§7.4) is accepted
// — this is the shape #2345's fix makes newly reachable (previously banned
// outright by #2318's blanket rule regardless of nullability). A primitive
// `int?` recipient is used (rather than a class type) to keep this test
// decoupled from an unrelated, pre-existing ownership gap: a PLAIN
// (non-owned) `out T` class-typed parameter whose body assigns `new T()`
// leaks even through an ordinary `.` call — confirmed during this fix's own
// verification, unrelated to `?.`, out of this issue's scope.
//
// Only the NON-null-receiver (call-ran) branch is exercised here. The
// null-receiver (call-skipped) branch of this exact shape hits a SEPARATE,
// PRE-EXISTING bug discovered while verifying this fix:
// `ensureOutVarSlots`'s plain "out var" branch (LlvmBackend_Calls2.cpp,
// ~line 1585) creates the recipient's slot via a bare `entryAlloca` with NO
// zero-initializing store — fine for a non-nullable recipient (DA forbids
// ever reading it unwritten, so the uninitialized bits are never observed),
// but WRONG for a nullable recipient, whose "hasValue" tag is supposed to
// default to false/null (§7.9's "a nullable local can be declared without
// an initializer (gets null)", which is exactly the guarantee §22.4's
// "default-initialized" claim for these recipients rests on) yet instead
// reads whatever garbage bits happened to be on the stack. Confirmed
// PRE-EXISTING and NOT introduced by this fix: this fix's diff to
// LlvmBackend_Calls2.cpp adds a new CALL SITE for `ensureOutVarSlots`
// (`lowerNullCondCall`'s first statement) but does not touch that
// function's internals, and the identical failure reproduces through
// PLAIN `and` short-circuiting with no `?.` involved at all —
// `if flag and C.TryParse(s, out var n: int?) then ... end; if n <> null
// then ... end;` with `flag = false` returns the "spuriously non-null"
// branch when compiled WITHOUT a sanitizer (a non-ASan build leaves true
// stack garbage; ASan's instrumentation happens to mask it, which is why an
// early probe of this exact case appeared to pass under `--asan` before
// this was traced to its root cause). Filed as #2395 rather than fixed
// here — it predates `?.`, is shared code (`lowerCall`'s own call site is
// untouched), and fixing it is out of #2345's DA-modeling scope per
// CLAUDE.md's no-"while-we're-at-it" discipline.
// =============================================================================

BEZEL_TEST(nullcond_out_da_2345,
           nullable_inline_out_var_null_guarded_read_nonnull_receiver) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Helper "
        "  public method Make(out a: int?) a := 42; end Make; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    hh?.Make(out var b); "
        "    if b <> null then return b!; end; "
        "    return 7; "
        "  end Main; end C;"), 42);
}

// =============================================================================
// Self-pass (c): a chained null-conditional receiver (`a?.B?.M(out n)`) has
// TWO short-circuit points — `a` being null and `a.B` being null both skip
// the call. Exercised across all three reachable nullness combinations with
// a PRE-DECLARED recipient (DA-transparent either way; this test is about
// the CHAIN's own short-circuit reaching the call correctly, composing with
// the accepted pre-declared shape from nullcond_ref_out_2318_test.cpp).
// =============================================================================

namespace {
const char* kChainClasses =
    "module M; "
    "class BClass public method M(out x: int) x := 7; end M; end BClass; "
    "class A public owned B: BClass? := null; end A; ";
}  // namespace

BEZEL_TEST(nullcond_out_da_2345, chained_nullcond_out_a_null) {
    BEZEL_EXPECT_EQ(run(std::string(kChainClasses) +
        "class C "
        "  public static method Main(): int "
        "    var a: A? := null; "
        "    var n := 99; "
        "    a?.B?.M(out n); "
        "    return n; "
        "  end Main; end C;"), 99);
}

BEZEL_TEST(nullcond_out_da_2345, chained_nullcond_out_a_nonnull_b_null) {
    BEZEL_EXPECT_EQ(run(std::string(kChainClasses) +
        "class C "
        "  public static method Main(): int "
        "    using var aa := new A(); "
        "    var a: A? := aa; "
        "    var n := 99; "
        "    a?.B?.M(out n); "
        "    return n; "
        "  end Main; end C;"), 99);
}

BEZEL_TEST(nullcond_out_da_2345, chained_nullcond_out_both_nonnull) {
    BEZEL_EXPECT_EQ(run(std::string(kChainClasses) +
        "class C "
        "  public static method Main(): int "
        "    using var aa := new A(); "
        "    aa.B := new BClass(); "
        "    var a: A? := aa; "
        "    var n := 99; "
        "    a?.B?.M(out n); "
        "    return n; "
        "  end Main; end C;"), 7);
}

// =============================================================================
// Self-pass (d): the null-forgiving `!` operator (§7.8) does NOT route
// through this fix at all — `hh!.M(out x)`'s callee is a plain
// `MemberAccessExpr` whose target is a `PostfixBangExpr`, never a
// `NullCondMemberExpr`. It is an ordinary `.` call: `out n` is
// unconditionally assigned, exactly as `plain_dot_call_ref_to_local_unaffected`
// (nullcond_ref_out_2318_test.cpp) already proves for `ref`.
// =============================================================================

BEZEL_TEST(nullcond_out_da_2345, null_forgiving_bang_is_a_plain_dot_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Helper "
        "  public method Set(out a: int) a := 99; end Set; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    var n := 10; "
        "    hh!.Set(out n); "
        "    return n; "
        "  end Main; end C;"), 99);
}

// =============================================================================
// Still-rejected shapes (#2393): an accessor-mediated (indexer-slot)
// pre-declared `out` target through `?.` — #1302's copy-in/copy-out bracket
// composed with `?.` is unverified for `out` (only `ref` is proven, in
// nullcond_ref_out_2318_test.cpp's indexer-slot tests) — and a bare
// identifier that resolves to a FIELD rather than a local/parameter.
// =============================================================================

BEZEL_TEST(nullcond_out_da_2345, out_to_indexer_slot_through_nullcond_is_rejected) {
    auto r = runAot(
        "module M; class Helper "
        "  public method Bump(out a: int) a := a + 5; end Bump; "
        "end Helper; class C "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    var hh: Helper? := h; "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    hh?.Bump(out arr[0]); "
        "    return arr[0]; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited22_3 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§22.3") != std::string::npos) cited22_3 = true;
    BEZEL_EXPECT_TRUE(cited22_3);
}

BEZEL_TEST(nullcond_out_da_2345, out_to_field_through_nullcond_is_rejected) {
    auto r = runAot(
        "module M; class Helper "
        "  public method Set(out a: int) a := 99; end Set; "
        "end Helper; class C "
        "  private _n: int := 10; "
        "  public method Run(hh: Helper?): int "
        "    hh?.Set(out _n); "
        "    return _n; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var h := new Helper(); "
        "    using var c := new C(); "
        "    return c.Run(h); "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited22_3 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§22.3") != std::string::npos) cited22_3 = true;
    BEZEL_EXPECT_TRUE(cited22_3);
}

// =============================================================================
// Control: `and`/`or`'s own short-circuit `out var` modeling (§22.4's
// worked example) is untouched by this fix — evalCond's `and`/`or`
// dynamic_cast branches are unmodified; only the CallExpr handling gained a
// new NullCondMemberExpr-callee case. Mirrors §22.4's own
// `if flag and parser.TryParse(s, out var n) then ... end;` example, using
// the same TryParse-shaped pattern the existing DA test suite exercises
// elsewhere (definite_assignment_test.cpp / da_out_var tests) — kept here
// as a local, self-contained smoke test so this file's `?.`-specific claim
// ("and/or unaffected") self-checks without cross-file coupling.
// =============================================================================

BEZEL_TEST(nullcond_out_da_2345, and_short_circuit_out_var_modeling_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := false; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      return n; "
        "    end; "
        "    return 7; "
        "  end Main; end C;"), 7);
}

BEZEL_TEST(nullcond_out_da_2345, and_short_circuit_out_var_true_path_reads_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryParse(s: string, out n: int): bool "
        "    n := 42; return true; "
        "  end TryParse; "
        "  public static method Main(): int "
        "    var flag := true; "
        "    if flag and C.TryParse(\"x\", out var n) then "
        "      return n; "
        "    end; "
        "    return 7; "
        "  end Main; end C;"), 42);
}
