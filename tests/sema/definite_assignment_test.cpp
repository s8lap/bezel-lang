// Spec-derived tests for Sema slice DA1 — definite-assignment for
// locals. Every read of a local must be preceded on every reaching
// path by at least one write. Reads of an unassigned local are
// diagnosed.

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class C public method M(cond: bool) ") + body + " end M; end C;";
}

// Scan diagnostics for the DA pass's "read of unassigned local" text —
// needed early in this file because the #4419 controls reuse it to confirm
// the rejection carries the §7.9-cited message (not some unrelated cascade).
bool mentionsUnassignedRead(const bezel::test::DAResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("read of unassigned local") != std::string::npos)
            return true;
    return false;
}

} // namespace

BEZEL_TEST(sema_da, local_with_init_is_assigned) {
    auto r = parseAndCheckDA(wrap("var x := 1; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419): a local of DEFAULTABLE type declared without an
// initializer (`var x: int;`) holds the type's default (`0` for int) from
// its declaration and may be read from any reaching path. Replaces the
// pre-#4419 over-strict verdict (which rejected every no-init local
// unconditionally). Pair with `local_nondefaultable_no_init_still_diagnoses`
// below, which preserves the rejection for non-defaultable types.
BEZEL_TEST(sema_da, local_defaultable_no_init_read_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var x: int; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419): a NON-defaultable local declared without an
// initializer (non-nullable `string`) still requires explicit assignment
// before read — the §17.11 "defaultable" glossary entry admits only
// nullable references and recursively-defaultable value types, so plain
// `string` stays over the line.
BEZEL_TEST(sema_da, local_nondefaultable_no_init_still_diagnoses_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; class C "
        "  public static method Make(): string return \"hi\"; end Make; "
        "  public method M() "
        "    var s: string; _ := s; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

BEZEL_TEST(sema_da, local_assigned_before_read_is_clean) {
    auto r = parseAndCheckDA(wrap("var x: int; x := 5; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, both_branches_assign_is_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "if cond then x := 1; else x := 2; end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) variant: a DEFAULTABLE-typed no-init local
// (`var x: int;`) reads the §17.11 default (`0`) on the if-branch that
// did not assign it — the read past the `if` is no longer a DA error.
// Replaces the pre-#4419 over-strict verdict (which rejected every
// no-init local unconditionally, regardless of how many branches assigned).
// Pair with `only_one_branch_assigns_nondefaultable_still_diagnoses` below,
// which preserves the rejection for a non-defaultable type.
BEZEL_TEST(sema_da,
           only_one_branch_assigns_defaultable_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "if cond then x := 1; end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) companion: the same one-branch shape with a
// NON-defaultable type — non-nullable `Object` is not in §17.11's
// defaultable list, so the unassigned branch keeps the `read of
// unassigned` rejection.
BEZEL_TEST(sema_da,
           only_one_branch_assigns_nondefaultable_still_diagnoses_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public method M(cond: bool) "
        "    var o: Object; "
        "    if cond then o := new Object(); end; "
        "    _ := o; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

BEZEL_TEST(sema_da, elif_branches_all_assign_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "if cond then x := 1; "
        "elif cond then x := 2; "
        "else x := 3; "
        "end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) variant: a DEFAULTABLE-typed no-init local
// (`var x: int;`) reads the §17.11 default (`0`) on the path where the
// while body never executed — the post-loop read is no longer a DA error.
// Replaces the pre-#4419 over-strict verdict (which rejected every
// no-init local unconditionally, regardless of how many loop iterations
// ran). Pair with `while_body_assigns_nondefaultable_doesnt_count_after`
// below, which preserves the rejection for a non-defaultable type.
BEZEL_TEST(sema_da,
           while_body_defaultable_no_assign_after_loop_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "while cond do x := 1; end; "
        "_ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) companion: the same while shape with a
// NON-defaultable type — non-nullable `Object` is not in §17.11's
// defaultable list, so the post-loop read on the zero-iteration path
// keeps the `read of unassigned` rejection.
BEZEL_TEST(sema_da,
           while_body_assigns_nondefaultable_doesnt_count_after_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public method M(cond: bool) "
        "    var o: Object; "
        "    while cond do o := new Object(); end; "
        "    _ := o; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

BEZEL_TEST(sema_da, while_body_read_after_assign_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x: int; "
        "while cond do x := 1; _ := x; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, param_is_always_assigned) {
    // Parameters are part of the method's entry state — implicit
    // assignment. Slice DA1 doesn't register them in declared_ so
    // reads pass without entering the unassigned-check at all.
    auto r = parseAndCheckDA(wrap("_ := cond;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, field_is_silent_pass_through) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  private _x: int := 42; "
        "  public method M() _ := this._x; end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, local_used_in_initializer_of_another_clean) {
    auto r = parseAndCheckDA(wrap(
        "var x := 1; var y := x + 1; _ := y;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_da, self_referential_init_reads_undeclared_diagnoses) {
    // `var x := x + 1;` reads x BEFORE its declaration completes →
    // unassigned read.
    auto r = parseAndCheckDA(wrap("var x := x + 1; _ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// fix #747 R43 / §22.3: a pre-declared local passed as `out x` is an OUTPUT
// location — "the variable need not be initialized before the call", and the
// callee assigns it on every exit path — so it is definitely assigned after the
// call. The DA pass previously treated every call argument as a read, so a
// `var v: int; M(out v); _ := v;` drew a false "read of unassigned" both at the
// argument and at the post-call read.
BEZEL_TEST(sema_da, out_arg_predeclared_local_is_assigned_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method GetIt(out v: int): bool v := 42; return true; end GetIt; "
        "  public static method Main(): int "
        "    var v: int; "
        "    var ok := GetIt(out v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) variant of the §22.3 control: a DEFAULTABLE-typed
// no-init local (`var v: int;`) reads the §17.11 default (`0`), so the
// plain `return v` after the declaration is NOT a definite-assignment
// error — the out-arg relaxation (which the original control was meant to
// confirm had not over-broadened into a free pass) does not even need to
// apply. Pair with `plain_nondefaultable_no_init_return_still_diagnoses`
// below, which preserves the rejection for non-defaultable types.
BEZEL_TEST(sema_da,
           plain_defaultable_no_init_return_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var v: int; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) companion: a NON-defaultable no-init local
// (non-nullable `string`) still requires explicit assignment before
// `return v` — the §17.11 glossary entry excludes non-nullable references,
// so the `return v` is a genuine read-of-unassigned.
BEZEL_TEST(sema_da,
           plain_nondefaultable_no_init_return_still_diagnoses_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Main(): string "
        "    var v: string; "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

// §22.3 control (companion to the plain-read pair above): a `ref` argument
// is read-write — the variable must be assigned BEFORE the call, so an
// unassigned `ref v` still diagnoses (only `out` is a pure write). Updated
// #4419 (#7.9/v0.299) variant uses a NON-defaultable type
// (non-nullable `Object`) so the §7.9 default-init relaxation does NOT
// apply — the original `int` shape no longer diagnoses after the fix.
BEZEL_TEST(sema_da, ref_arg_nondefaultable_still_diagnoses_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Inc(ref v: Object) v := v; end Inc; "
        "  public static method Main(): int "
        "    var v: Object; "
        "    Inc(ref v); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

// §7.9 (v0.299, #4419) companion: `ref` on a DEFAULTABLE no-init local
// (`var v: int;`) — v has the §17.11 default (`0`) at the call, so the
// read side of the `ref` is satisfied and the call is legal. (This is the
// pre-#4419 case that the prior text over-rejected.)
BEZEL_TEST(sema_da, ref_arg_defaultable_no_init_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Inc(ref v: int) v := v + 1; end Inc; "
        "  public static method Main(): int "
        "    var v: int; "
        "    Inc(ref v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- fix #925: callee-side out-parameter definite-assignment (§22.3) -------

// §22.3 / #925: a callee that never assigns its out-parameter on any exit path
// must be rejected with a compile error naming the unassigned param.
BEZEL_TEST(sema_da, callee_out_never_assigned_rejects_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method NeverAssigns(out x: int) "
        "    // x never assigned "
        "  end NeverAssigns; "
        "  public static method Main(): int "
        "    var v: int := 42; "
        "    C.NeverAssigns(out v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a callee that assigns the out-param only in the `if` branch
// (no `else`) fails the §22.3 check — the path where the condition is false
// exits without assigning.
BEZEL_TEST(sema_da, callee_out_partial_if_no_else_rejects_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method MaybeAssign(cond: bool, out x: int) "
        "    if cond then x := 7; end; "
        "    // x not assigned on the !cond fall-through path "
        "  end MaybeAssign; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a callee that returns early (before assigning the out-param)
// fails the check — the early return is an exit path where x is unassigned.
BEZEL_TEST(sema_da, callee_out_early_return_before_assign_rejects_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method EarlyReturn(cond: bool, out x: int) "
        "    if cond then return; end; "   // x not assigned at this return
        "    x := 7; "
        "  end EarlyReturn; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a callee that assigns the out-param on every path compiles
// cleanly — no error, and the callee-assigned value is observable.
BEZEL_TEST(sema_da, callee_out_assigns_all_paths_is_clean_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method GetIt(out x: int) x := 7; end GetIt; "
        "  public static method Main(): int "
        "    var v: int := 0; "
        "    C.GetIt(out v); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / #925: assigning the out-param in BOTH branches of an if/else before
// fall-off satisfies the check.
BEZEL_TEST(sema_da, callee_out_if_else_both_assign_is_clean_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Choose(cond: bool, out x: int) "
        "    if cond then x := 1; else x := 2; end; "
        "  end Choose; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / #925: a `ref` parameter that the callee reads but does not
// re-assign must still compile cleanly — `ref` starts assigned (the caller
// passes an initialised lvalue), so no out-param check applies.
BEZEL_TEST(sema_da, callee_ref_param_not_reassigned_is_clean_spec_22_3_925) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method ReadOnly(ref n: int): int return n; end ReadOnly; "
        "  public static method Main(): int "
        "    var v: int := 42; "
        "    return C.ReadOnly(ref v); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// -----------------------------------------------------------------------------
// §22.4 / §7.9 (v0.181, #1552): inline `out var` definite assignment is
// short-circuit path-sensitive. An inline out variable is definitely assigned
// only on control-flow paths where the introducing call actually ran; under
// `and`/`or` the distinction is live. A read of a non-nullable-reference-typed
// out var on a path where the call was skipped is a compile error; guarded reads
// stay accepted; value-typed and nullable out vars follow §7.9 default-init and
// remain readable on every path.
// -----------------------------------------------------------------------------

// (The mentionsUnassignedRead helper now lives in the top namespace near
// `wrap` — its definition was moved there so the #4419 controls near the
// top of the file can reuse it before the §22.4 section that originally
// introduced it.)

// (a) REJECT: `if flag and TryGet(out var s) then ... end; _ := s;` — on the
// `flag == false` path the call never ran, so a read of the non-nullable
// reference (`string`) out var `s` after the `if` is a definite-assignment
// error. This is the accept+segfault the issue reported.
BEZEL_TEST(sema_da,
           inline_out_var_short_circuit_read_after_if_rejected_spec_22_4_1552) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGet(out v: string): bool "
        "    v := \"hi\"; return true; "
        "  end TryGet; "
        "  public static method Run(flag: bool) "
        "    if flag and TryGet(out var s) then _ := s; end; "
        "    _ := s; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

// (b) ACCEPT: the idiomatic guarded read — `s` is read only inside the `then`
// branch, reached only when the `and`-conjunct call ran and returned `true`, so
// `s` is definitely assigned there (§22.4 after-true fact).
BEZEL_TEST(sema_da,
           inline_out_var_short_circuit_guarded_read_accepted_spec_22_4_1552) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGet(out v: string): bool "
        "    v := \"hi\"; return true; "
        "  end TryGet; "
        "  public static method Run(flag: bool) "
        "    if flag and TryGet(out var s) then _ := s; end; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) ACCEPT: a value-typed inline out var (`int`) is default-initialized
// (§7.9); reading it after a short-circuited call observes the default, not
// garbage, so the read stays accepted.
BEZEL_TEST(sema_da,
           inline_out_var_value_type_read_after_short_circuit_accepted_spec_7_9_1552) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGetInt(out n: int): bool "
        "    n := 42; return true; "
        "  end TryGetInt; "
        "  public static method Run(flag: bool) "
        "    if flag and TryGetInt(out var n) then end; "
        "    _ := n; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c') ACCEPT: a nullable-reference inline out var (`string?`) is
// default-initialized to `null` (§7.9); the read after a short-circuited call
// observes `null`, so it stays accepted (the declared nullability must survive
// the type checker's reference-`?`-strip — #1552).
BEZEL_TEST(sema_da,
           inline_out_var_nullable_read_after_short_circuit_accepted_spec_7_9_1552) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGetN(out v: string?): bool "
        "    v := \"hi\"; return true; "
        "  end TryGetN; "
        "  public static method Run(flag: bool) "
        "    if flag and TryGetN(out var s) then end; "
        "    _ := s; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (d) ACCEPT: when the whole guard IS the call (no short-circuit above it), the
// call ran on every path reaching past the `if`, so the non-nullable out var is
// definitely assigned afterward.
BEZEL_TEST(sema_da,
           inline_out_var_unconditional_call_read_after_if_accepted_spec_22_4_1552) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGet(out v: string): bool "
        "    v := \"hi\"; return true; "
        "  end TryGet; "
        "  public static method Run() "
        "    if TryGet(out var s) then end; "
        "    _ := s; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (e) REJECT: `or` is the mirror image of `and` — `A or Call(out var x)` assigns
// `x` only when the whole guard is FALSE, i.e. on the `else`/false path. A read
// of `x` in the `then` branch (guard true, which includes the `A == true` path
// on which the call was skipped) is a definite-assignment error.
BEZEL_TEST(sema_da,
           inline_out_var_or_short_circuit_then_read_rejected_spec_22_4_1552) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  static method TryGet(out v: string): bool "
        "    v := \"hi\"; return true; "
        "  end TryGet; "
        "  public static method Run(flag: bool) "
        "    if flag or TryGet(out var s) then _ := s; end; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

// -----------------------------------------------------------------------------
// fix #1681 / §22.3, §19 (v0.184): out-param definite-assignment inside a
// statement-position `case` (§19). Before the fix DefiniteAssignment.cpp had no
// visitor branch for ast::CaseStmt at all — the walker treated the whole
// construct as an inert no-op, so every statement inside a case arm (including
// `return`s and out-param assignments) was invisible to DA. That simultaneously
// (a) wrongly ACCEPTED a `return` inside one arm that never assigned the
// out-param whenever some other statement before/after the case satisfied the
// method's fall-off check, and (b) wrongly REJECTED an exhaustively-assigning
// case body (the fall-off check fired unconditionally, since assigned_ never
// picked up any arm's assignment).
// -----------------------------------------------------------------------------

// (a) REJECT — the exact under-reject #1681 reports: the `1` arm returns without
// assigning `result`; a later ordinary statement re-assigns it before the
// method's final `return`, which is what let this compile cleanly before the
// fix (the old fall-off-only check saw `result` assigned by then and never
// looked inside the case arms). Every arm's own return path must independently
// satisfy §22.3.
BEZEL_TEST(sema_da, case_arm_return_without_out_assign_rejected_spec_22_3_1681) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method TryFoo(n: int, out result: int): bool "
        "    case n of "
        "      1 => do return true; end, "
        "      _ => do result := 5; return false; end "
        "    end; "
        "    result := 99; "
        "    return true; "
        "  end TryFoo; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (b) ACCEPT — positive control: every case arm assigns `result` before its own
// return, and the case is exhaustive (an unguarded `_` catch-all, §19.6).
BEZEL_TEST(sema_da, case_all_arms_assign_out_before_return_accepted_spec_22_3_1681) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method TryFoo(n: int, out result: int): bool "
        "    case n of "
        "      1 => do result := 1; return true; end, "
        "      2 => do result := -1; return false; end, "
        "      _ => do result := 0; return false; end "
        "    end; "
        "  end TryFoo; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (c) REJECT — one arm of an otherwise-fully-assigning, exhaustive case never
// assigns `result` on its own return path; the diagnostic must fire regardless
// of what the other arms do.
BEZEL_TEST(sema_da, case_one_arm_out_unassigned_rejected_spec_22_3_1681) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method TryFoo(n: int, out result: int): bool "
        "    case n of "
        "      1 => do result := 1; return true; end, "
        "      2 => do return false; end, "
        "      _ => do result := 0; return false; end "
        "    end; "
        "  end TryFoo; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// (d) ACCEPT — the simplest exhaustive shape: a single unguarded `_` arm that
// assigns `result` before its own `return`.
BEZEL_TEST(sema_da, case_single_wildcard_arm_assigns_out_accepted_spec_22_3_1681) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method GetIt(n: int, out result: int): bool "
        "    case n of "
        "      _ => do result := n; return true; end "
        "    end; "
        "  end GetIt; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (e) REJECT — §19.6: a case with no provable catch-all arm carries an implicit
// "no match" fall-through path that assigns nothing to `result`; that live path
// must also satisfy §22.3 even though the one declared arm assigns before its
// own return.
BEZEL_TEST(sema_da,
           case_no_catchall_implicit_fallthrough_rejected_spec_19_6_22_3_1681) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method TryFoo(n: int, out result: int): bool "
        "    case n of "
        "      1 => do result := 1; return true; end "
        "    end; "
        "    return false; "
        "  end TryFoo; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// -----------------------------------------------------------------------------
// fix #1753 / §17.x, §22.3 (v0.184): the if/try/case JOIN must EXCLUDE branches
// that provably diverge (throw/return) from the definite-assignment
// intersection. A local assigned on every REACHING path is definitely assigned;
// a branch that returns/throws reaches no join, so it must not shrink the
// intersection. Before the fix, the terminated branch's (unassigned) state was
// intersected in and a following read was wrongly reported unassigned. The
// exclusion must NOT under-reject: a branch that FALLS THROUGH without assigning
// still shrinks the intersection and must still be caught. Sibling of review-
// filed #1752; closes #1753.
// -----------------------------------------------------------------------------

// ACCEPT — the else branch `return`s, so on every reaching path to `_ := x`, `x`
// is assigned. Wrongly rejected before the fix.
BEZEL_TEST(sema_da, if_else_returns_read_after_is_assigned_accepted_spec_17_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(c: bool): int "
        "    var x: int; "
        "    if c then x := 1; else return 0; end; "
        "    return x; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT — the else branch `throw`s (also a diverging exit); same reasoning.
BEZEL_TEST(sema_da, if_else_throws_read_after_is_assigned_accepted_spec_17_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(c: bool): int "
        "    var x: int; "
        "    if c then x := 1; else throw new Exception(); end; "
        "    return x; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) variant of the §17 under-reject guard: a
// DEFAULTABLE no-init local (`var x: int;`) reads the §17.11 default (`0`)
// on the reaching path that did not assign it — the if/else
// terminated-branch-exclusion logic itself is unchanged, but the per-path
// assignment check no longer fires for a defaultable local. Pair with
// `if_else_live_nondefaultable_without_assign_still_rejected` below,
// which preserves the rejection for a non-defaultable type.
BEZEL_TEST(sema_da,
           if_else_live_defaultable_without_assign_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(c: bool): int "
        "    var x: int; "
        "    if c then x := 1; else _ := c; end; "
        "    return x; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) companion: the same shape with a NON-defaultable
// type — non-nullable `string` is not in §17.11's defaultable list, so a
// live else branch that does not assign `s` keeps the `read of unassigned`
// rejection (this is the case the original §17 control was written to
// pin). The terminated-branch exclusion must not swallow this genuine
// error.
BEZEL_TEST(sema_da,
           if_else_live_nondefaultable_without_assign_still_rejected_spec_17) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(c: bool): string "
        "    var s: string; "
        "    if c then s := \"a\"; else _ := c; end; "
        "    return s; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}

// ACCEPT — the terminated-branch exclusion in a `case` join: the `2` arm
// `return`s (diverges, excluded), while both other live arms assign `x`, so `x`
// is assigned on every reaching path after the case.
BEZEL_TEST(sema_da, case_terminated_arm_excluded_read_after_accepted_spec_19_1753) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(n: int): int "
        "    var x: int; "
        "    case n of "
        "      2 => do return 0; end, "
        "      _ => do x := 1; end "
        "    end; "
        "    return x; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) variant of the §19 under-reject guard: a
// DEFAULTABLE no-init local (`var x: int;`) reads the §17.11 default
// (`0`) on the live arm that did not assign it — the `case` join's
// terminated/live-arm accounting itself is unchanged, but the per-path
// assignment check no longer fires for a defaultable local. Pair with
// `case_live_arm_nondefaultable_without_assign_still_rejected` below,
// which preserves the rejection for a non-defaultable type.
BEZEL_TEST(sema_da,
           case_live_arm_defaultable_without_assign_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(n: int): int "
        "    var x: int; "
        "    case n of "
        "      2 => do _ := n; end, "
        "      _ => do x := 1; end "
        "    end; "
        "    return x; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.9 (v0.299, #4419) companion: the same `case` join shape with a
// NON-defaultable type — non-nullable `string` is not in §17.11's
// defaultable list, so a live arm that does not assign `s` keeps the
// `read of unassigned` rejection (this is the case the original §19
// control was written to pin).
BEZEL_TEST(sema_da,
           case_live_arm_nondefaultable_without_assign_still_rejected_spec_19) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public static method Pick(n: int): string "
        "    var s: string; "
        "    case n of "
        "      2 => do _ := n; end, "
        "      _ => do s := \"x\"; end "
        "    end; "
        "    return s; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(mentionsUnassignedRead(r));
}
