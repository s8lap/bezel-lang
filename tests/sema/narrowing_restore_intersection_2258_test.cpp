// Spec-derived tests for issue #2258 — a §7.4 narrowing "restore" must be an
// INTERSECTION with the saved snapshot, never an assignment of it.
//
// §7.4 (v0.228), "How narrowing is invalidated": the narrowing of a variable
// is invalidated when, within the controlled block, "the variable is
// reassigned from a nullable-typed expression", "the variable is passed as a
// `ref` **or `out`** argument", or "the variable is **mutably captured** … and
// the block performs *any* operation that can transfer control to user code".
//
// Three save/restore sites wrote the restore as an ASSIGNMENT of the entry
// snapshot, which RESURRECTS every fact the visited sub-expression had just
// invalidated by one of those forms — so a null flowed into a slot the checker
// believed non-null. Each was a silent compile-ACCEPT ending in SIGSEGV, never
// a diagnostic — probe-confirmed for (1) in both positions, (2), (3), and (4):
// AddressSanitizer "SEGV on unknown address 0x000000000008", the string-length
// load off a null receiver:
//
//   (1) the `case`/`when` ARM restore — ExprTypecheck.cpp (CaseStmt) and
//       ExprTypecheck_Exprs.cpp (CaseExpr). It additionally ran BEFORE the
//       #2116/#2164 running-intersection fold, so the fold intersected
//       `caseRunning*` with itself (a no-op) and the arm's REAL exit state
//       was discarded. The branch is taken by every arm that is not a
//       BindingPattern or DestructuringPattern arm: constant, `_` discard,
//       enum-constant, relational, alt, and scrutinee-less boolean arms.
//   (2) the restore after the RHS of `and` (#2212's narrowing seed) —
//       ExprTypecheck_Exprs.cpp's `visitChildren` BinaryExpr path.
//   (3) the §20 conditional atom's entry restore (#2105) —
//       ExprTypecheck_Exprs.cpp's ConditionalExpr path. Its comment claimed
//       "the post-`if` expression state is irrelevant (a conditional atom
//       yields one of the two branch values, with no following statements on
//       either path)", which is false: a branch VALUE can be a call, and
//       execution continues after the atom. The sound after-state is
//       `entry ∩ thenExit ∩ elseExit`.
//
// Plus two adjacent defects in the same code, both probe-confirmed as silent
// ACCEPTs:
//   (4) the choice-destructure arm path `continue`s past the per-arm fold, so
//       that arm's exit — including its DROPS — never reached the after-case
//       state (SIGSEGV, as above).
//   (5) the conditional atom reset `nullNarrowed_`/`fieldNarrowed_` per branch
//       but NOT the §12.7 type map, so the `then` branch's `b is Derived` fact
//       was still live while the ELSE value was checked — a wrong-typed
//       dispatch compiled on the path where the test FAILED.
//
// EVERY reject test below is in the INVALIDATION-LOSS direction (does a DROP
// survive the construct?) — deliberately, because the shipped #2105/#2116/
// #2164/#2212 tests all exercise only the ESTABLISHMENT-LEAK direction (does a
// fact ESCAPE the construct?), which the buggy assignment-restore also
// happened to suppress. That is why none of them caught any of these. Each
// test here compiles clean before the fix and is rejected after it.
//
// The ACCEPT controls at the bottom guard the other side: an intersection is
// strictly more conservative than the assignment it replaces, so it must not
// cost any legal program its narrowing.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A `ref`-taking helper that nulls its argument — the §7.4 invalidation form
// "the variable is passed as a `ref` or `out` argument", spelled once.
const char* kClearDecls =
    "  public static method Clear(ref s: string?): bool "
    "    s := null; return true; "
    "  end Clear; "
    "  public static method ClearI(ref s: string?): int "
    "    s := null; return 7; "
    "  end ClearI; ";

}  // namespace

// ---------------------------------------------------------------------------
// (1) case/when ARM restore — the arm's DROP must survive into the after-case
//     state (§7.4 invalidation + §7.4 #2116/#2164 running intersection).
// ---------------------------------------------------------------------------

// §7.4 (#2258): a CONSTANT arm whose body reassigns the narrowed local from a
// nullable expression invalidates the fact; the after-case state must not
// carry it. Pre-fix: `narrowings_/nullNarrowed_/fieldNarrowed_ = armEntry*`
// put the fact back and `x.Length` compiled — SIGSEGV for k = 1.
BEZEL_TEST(narrowing_restore_intersection_2258,
           case_stmt_constant_arm_assignment_drop_survives_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method P(k: int, x: string?): int "
        "    if x = null then return -1; end; "
        "    case k of "
        "      1 => do x := null; end, "
        "      _ => do Console.WriteLine(\"other\"); end "
        "    end; "
        "    return x.Length; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §7.4 (#2258): the same for the `ref`-argument invalidation form, in the `_`
// DISCARD arm — the second half of the "every non-binding, non-destructuring
// arm takes this branch" blast radius.
BEZEL_TEST(narrowing_restore_intersection_2258,
           case_stmt_discard_arm_ref_drop_survives_spec_7_4) {
    auto r = parseSemaAll(
        std::string("module M; class C ") + kClearDecls +
        "  public static method P(k: int, t: string?): int "
        "    var s: string? := t; "
        "    if s = null then return -1; end; "
        "    case k of "
        "      1 => do Console.WriteLine(\"one\"); end, "
        "      _ => do var b: bool := C.Clear(ref s); end "
        "    end; "
        "    return s.Length; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §7.4 (#2258): the EXPRESSION-position `case` has its own copy of the arm
// restore (ExprTypecheck_Exprs.cpp). An arm RESULT expression that passes the
// narrowed local as `ref` invalidates it for everything after the case.
BEZEL_TEST(narrowing_restore_intersection_2258,
           case_expr_constant_arm_ref_drop_survives_spec_7_4) {
    auto r = parseSemaAll(
        std::string("module M; class C ") + kClearDecls +
        "  public static method P(k: int, t: string?): int "
        "    var s: string? := t; "
        "    if s = null then return -1; end; "
        "    var v: int := (case k of "
        "      1 => C.ClearI(ref s), "
        "      _ => 0); "
        "    return s.Length + v; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// ---------------------------------------------------------------------------
// (2) `and` — the RHS's invalidation must survive the operator (§7.4
//     "boolean composition of tests" seeds the RHS; it must not un-drop).
// ---------------------------------------------------------------------------

BEZEL_TEST(narrowing_restore_intersection_2258,
           and_rhs_ref_drop_survives_the_operand_spec_7_4) {
    auto r = parseSemaAll(
        std::string("module M; class C ") + kClearDecls +
        "  public static method Run(t: string?): int "
        "    var s: string? := t; "
        "    if s = null then return 0; end; "
        "    var b: bool := true and C.Clear(ref s); "
        "    return s.Length; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// ---------------------------------------------------------------------------
// (3) §20 conditional atom — the after-atom state is entry ∩ thenExit ∩
//     elseExit, so a drop in EITHER branch survives the atom.
// ---------------------------------------------------------------------------

BEZEL_TEST(narrowing_restore_intersection_2258,
           conditional_atom_then_branch_ref_drop_survives_spec_7_4) {
    auto r = parseSemaAll(
        std::string("module M; class C ") + kClearDecls +
        "  public static method Run(t: string?, c: bool): int "
        "    var s: string? := t; "
        "    if s = null then return 0; end; "
        "    var n: int := (if c then C.ClearI(ref s) else 0); "
        "    return s.Length + n; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

BEZEL_TEST(narrowing_restore_intersection_2258,
           conditional_atom_else_branch_ref_drop_survives_spec_7_4) {
    auto r = parseSemaAll(
        std::string("module M; class C ") + kClearDecls +
        "  public static method Run(t: string?, c: bool): int "
        "    var s: string? := t; "
        "    if s = null then return 0; end; "
        "    var n: int := (if c then 0 else C.ClearI(ref s)); "
        "    return s.Length + n; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// ---------------------------------------------------------------------------
// (4) the choice-destructure arm path `continue`d past the per-arm fold.
// ---------------------------------------------------------------------------

// §7.4 / §35.5 (#2258): a `Some(var v) =>` arm is handled by the choice-
// destructure path, which left the arm loop with `continue` BEFORE the
// per-arm fold — so the arm's `s := null;` never shrank the running
// intersection and the after-case state still claimed `s` non-null.
BEZEL_TEST(narrowing_restore_intersection_2258,
           case_stmt_choice_destructure_arm_drop_is_folded_spec_7_4) {
    auto r = parseSemaAll(
        "module M; choice Opt Some(value: int), None; end Opt; "
        "class C "
        "  public static method P(m: Opt, t: string?): int "
        "    var s: string? := t; "
        "    if s = null then return 0; end; "
        "    case m of "
        "      Some(var v) => do s := null; end, "
        "      None => do Console.WriteLine(\"none\"); end "
        "    end; "
        "    return s.Length; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// ---------------------------------------------------------------------------
// (5) §12.7 type narrowing must not leak from the atom's THEN branch into its
//     ELSE branch — the else value is checked on the path where the test
//     FAILED, so the pattern type is not the operand's type there.
// ---------------------------------------------------------------------------

// §12.7 / §7.4 (#2258): `(if b is Derived then 1 else b.M())` — `M` is
// declared on `Derived` only. Pre-fix the atom reset the two null/field sets
// per branch but not `narrowings_`, so the else value resolved `b.M()` against
// `Derived` and compiled a wrong-typed dispatch on a `Base` value.
BEZEL_TEST(narrowing_restore_intersection_2258,
           conditional_atom_type_narrowing_does_not_leak_into_else_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base "
        "  public method M(): int return 7; end M; "
        "end Derived; "
        "class C "
        "  public static method Run(b: Base): int "
        "    return (if b is Derived then 1 else b.M()); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "no member `M` on type `Base`"));
}

// ---------------------------------------------------------------------------
// ACCEPT controls — an intersection can only ever narrow LESS than the
// assignment it replaces, so every legal program must keep compiling.
// ---------------------------------------------------------------------------

// §7.4: no arm invalidates anything, so the entry fact survives the case and
// the following `s.Length` is still accepted (the #2116/#2164 after-case
// intersection is entry ∩ every arm exit = entry).
BEZEL_TEST(narrowing_restore_intersection_2258,
           accept_case_stmt_without_drops_keeps_narrowing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method P(k: int, s: string?): int "
        "    if s = null then return 0; end; "
        "    case k of "
        "      1 => do Console.WriteLine(\"one\"); end, "
        "      _ => do Console.WriteLine(\"other\"); end "
        "    end; "
        "    return s.Length; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 / §20: a conditional atom that invalidates nothing leaves the entry
// narrowing intact (entry ∩ thenExit ∩ elseExit = entry).
BEZEL_TEST(narrowing_restore_intersection_2258,
           accept_conditional_atom_without_drops_keeps_narrowing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?, c: bool): int "
        "    if s = null then return 0; end; "
        "    var n: int := (if c then 1 else 0); "
        "    return s.Length + n; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 "boolean composition of tests": `x <> null and x.Length > 0` is
// well-typed — the #2212 seed of the RHS is untouched by the intersection
// (the seeded fact is discarded after the operator either way).
BEZEL_TEST(narrowing_restore_intersection_2258,
           accept_and_composition_seeds_rhs_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): bool "
        "    return s <> null and s.Length > 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4: an `and` whose RHS invalidates nothing leaves the entry fact intact
// for the statements that follow.
BEZEL_TEST(narrowing_restore_intersection_2258,
           accept_and_without_drops_keeps_narrowing_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?, c: bool): int "
        "    if s = null then return 0; end; "
        "    var b: bool := c and s.Length > 0; "
        "    return s.Length; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 / §20 (#2105 control): the atom's condition still narrows its own THEN
// branch — the fix changes only what survives PAST the atom.
BEZEL_TEST(narrowing_restore_intersection_2258,
           accept_conditional_atom_condition_narrows_then_branch_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    return (if s <> null then s.Length else 0); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.7 / §20 (control for the (5) reject): the type narrowing DOES apply
// inside the branch the successful test controls.
BEZEL_TEST(narrowing_restore_intersection_2258,
           accept_conditional_atom_type_narrowing_in_then_branch_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base "
        "  public method M(): int return 7; end M; "
        "end Derived; "
        "class C "
        "  public static method Run(b: Base): int "
        "    return (if b is Derived then b.M() else 0); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
