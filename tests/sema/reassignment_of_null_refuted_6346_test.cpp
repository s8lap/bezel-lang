// #6346 (§7.4, §8.4, §6.6, v0.338) — a TEST-ONLY commit: the issue's premise was
// measured and REFUTED, so what belongs in the tree is the measured rule, pinned
// so the premise cannot be re-reported as if it were a live defect.
//
// THE REPORT. #6346 claimed that "a local's §7.4 narrowing survives a later
// assignment of null, for every collection family", with this repro:
//
//     var n: Array<Item>? := new Array<Item>();
//     n.Reverse();      // admitted: §7.4 narrowed by the initialiser
//     n = null;         // the narrowing is now dead
//     n.Reverse();      // ADMITTED TOO — dispatches on null
//
// and read `n = null;` as "a write of the narrowable value" that "should drop
// the narrowing the same way" a loop back edge, a condition or a join does.
//
// WHAT WAS MEASURED, and it contradicts the report on its central premise.
//
//   1. `n = null;` IS NOT AN ASSIGNMENT. It is an EXPRESSION STATEMENT whose
//      expression is the EQUALITY operator of §17.3, whose semantics §8.4
//      states: `a = b` invokes `a.Equals(b)` with a null-safe wrapper and
//      yields a `bool`. Assignment is a different production — grammar A.6's
//      `AssignmentStmt`, spelled `:=` (with the compound family and `??=`);
//      `=` is not among its operators.
//      MEASURED in the implementation: the parser's assignment-operator list in
//      src/parser/Parser_Stmts.cpp admits `:=`, `+=`, `-=`, `*=`, `/=`, `mod=`,
//      `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=` — there is no `"="` in it — so
//      `n = null;` never becomes an `ast::AssignmentStmt` at all.
//
//   2. SO IT WRITES NOTHING, and the narrowing it "failed to drop" was never
//      stale. #6346 r2 below is the runtime proof: a program that stores a
//      Bool answer to "is `n` null?" right after `n = null;` prints nothing,
//      while the identical program with `n := null;` prints that `n` IS null.
//      The issue's own program is not a null dereference — it RUNS and prints
//      its marker, because `n` still holds the array.
//
//   3. THE REAL ASSIGNMENT SPELLING IS ALREADY ENFORCED. `n := null;` DOES
//      invalidate the narrowing, and the following `n.Reverse()` IS rejected
//      with §7.4's diagnostic — for BOTH collection families, for `??=` too,
//      and for a value-typed carrier (`string?`) exactly as for a reference one.
//      §7.4 "How narrowing is invalidated" lists "the variable is reassigned
//      from a nullable-typed expression"; the implementation discharges that
//      through `narrowOnAssignment`, which erases the fact on a nullable-typed
//      value and which the `:=` / `??=` arm of the assignment-statement walk
//      calls. That path was already correct before this commit.
//
//   4. "FOR EVERY COLLECTION FAMILY" IS TRUE BUT VACUOUS here. §7.4's carriers
//      are local variables, non-`ref` parameters, and `readonly` fields, and
//      `=`-assignment can only target a mutable field, a property, an indexer
//      slot — none of which §7.4 narrows at all ("It never narrows mutable
//      fields, properties, or `ref` parameters"). A `readonly` field is not
//      assignable outside its construction window, and inside that window the
//      rule does not apply. There is therefore NO §7.4 narrowing carrier that
//      `=` can ever assign to: the reported shape is not merely untested, it is
//      UNREACHABLE. The asymmetry the issue measured is an artefact of its
//      repro spelling `=` where the language's assignment spelling is `:=`.
//
// So there is no `src/` change for #6346. These rows exist so the measured rule
// is a pinned contract rather than a recollection.
//
// WHAT IS DELIBERATELY NOT PINNED. Whether the compiler should diagnose a bare
// statement that evaluates a discarded `bool` (`n = null;`, or
// `Console.ReadLine();`) is a SEPARATE design question this commit does not
// answer and does not claim to close. MEASURED on this tree: a bare statement
// discarding an ordinary non-owned value is silently accepted
// (`Console.ReadLine();` compiles clean), so accepting `n = null;` is
// CONSISTENT with how the language treats every other discarded value — it is
// not an inconsistency, and no diagnostic is owed by §7.4.
//
// RELATIONSHIP TO #6333. #6333 measured the receiver rule for both families and
// was refuted; #6346 is its filing lane's stated "residual that IS real", and
// this commit closes that residual by measurement rather than by a fix. See the
// forward-reference note in
// tests/sema/collection_receiver_narrowing_refuted_6333_test.cpp.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §7.4 diagnostic a nullable receiver produces, in the part the instance
// form and the static-call sugar share. The same needle the #6333 rows use;
// MEASURED here on the post-reassignment programs, so it is the rule under test
// and not a recollection.
const char* const kNarrowNeedle = "requires narrowing it to non-null first";

// Both families, so no row can be asymmetric by accident. `Array<T>` is stdlib
// §4.4 and `OwningArray<T>` is §4.5. The issue reported per-family symmetry as
// the finding; running every row for both is what turns that into a claim the
// tree actually holds.
const char* const kFamilies[] = {"Array", "OwningArray"};

// Substitute the family into every `FAMILY` slot in `src`. Applied to the
// WHOLE assembled program, not to one part of it: an earlier draft substituted
// only the class preamble and left the `use` snippet alone, so a row whose use
// named the type a second time (`FAMILY.Reverse(n);`) silently kept `Array`
// there and the OwningArray cell failed on a type mismatch rather than on
// §7.4 — MEASURED, and a row that would have asserted the wrong rule.
std::string withFamily(const char* family, const std::string& src) {
    std::string out = src;
    const std::string slot = "FAMILY";
    std::string::size_type at = out.find(slot);
    while (at != std::string::npos) {
        out.replace(at, slot.size(), family);
        at = out.find(slot, at + std::string(family).size());
    }
    return out;
}

// A local whose initialiser narrowed it (§7.4), then reassigned from `null` by
// the given statement, then a member call on it. The member call MUST be
// rejected with §7.4's diagnostic — not by crashing and not with an unrelated
// error. Both the receiver and the `use` snippet are substituted with the same
// family, so a row that names the type twice cannot quietly mix the two.
void expectRejectedAfterReassignment(const char* family,
                                    const std::string& reassign,
                                    const std::string& use) {
    auto r = runAotChecked(
        withFamily(family,
            "module M;\n"
            "public class Item\nend Item;\n"
            "public class Program\n"
            "  public static method Main(): int\n"
            "    var n: FAMILY<Item>? := new FAMILY<Item>();\n"
            "    n.Reverse();\n"
            "    " + reassign + "\n" + use + "\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNarrowNeedle));
}

}  // namespace

// #6346 r1: THE REFUTATION, in the language's own terms. §7.4's rule is
// sound here because the program the issue filed does not perform the write it
// reports. This row is the base of every other row in the file: if `=` ever
// became an assignment operator, this row fails and the rest must be re-derived.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           equality_operator_does_not_assign_so_it_leaves_the_narrowing_alive_spec_7_4_spec_8_4) {
    // `n = null;` is §8.4 equality used as a statement. It stores nothing, so
    // `n` still holds the array and the narrowing established by the
    // initialiser is still TRUE — keeping it is the correct answer, and the
    // member call that follows is not a null dereference.
    auto r = runAotChecked(
        "module M;\n"
        "public class Item\nend Item;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var n: Array<Item>? := new Array<Item>();\n"
        "    n.Reverse();\n"
        "    n = null;\n"          // equality, not assignment
        "    n.Reverse();\n"       // still sound: `n` was never set to null
        "    Console.WriteLine(\"MARKER\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    // Asserting the MARKER is what makes this row more than a compile check: it
    // proves the program RAN to completion, so the second `n.Reverse()`
    // dispatched on a real array. Had `=` stored, that call would have
    // dispatched on null.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("MARKER") != std::string::npos);
}

// #6346 r2: THE RUNTIME PROOF that `n = null;` is not a store — the row that
// distinguishes this issue from a real one. The program asks, at runtime,
// whether `n` is null. With `=` it is NOT (no output); the control case with
// `:=` below is the twin that says it IS. Pinning both together is what stops a
// future reader from reasoning about `=` as though it assigned.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           equality_leaves_the_local_non_null_while_assignment_makes_it_null_spec_7_4_spec_8_4) {
    const std::string body =
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var n: Array<Item>? := new Array<Item>();\n"
        "    n.Reverse();\n"
        "    REASSIGN\n"
        "    if n = null then Console.WriteLine(\"IS-NULL\"); end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    const std::string prefix = "module M;\npublic class Item\nend Item;\n";

    // `=` — equality. `n` is NOT null afterwards, so nothing is printed. An
    // assertion on ABSENCE of output is safe here only because the control leg
    // below prints on the SAME program shape, so the oracle is not vacuous: if
    // the compiler ever stopped evaluating `n`, this leg would also go quiet
    // and the control leg would still print.
    {
        std::string src = prefix + body;
        const std::string k = "REASSIGN";
        src.replace(src.find(k), k.size(), "n = null;");
        auto r = runAotChecked(src);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_TRUE(r.stdoutText.find("IS-NULL") == std::string::npos);
    }
    // `:=` — the assignment. `n` IS null afterwards, so the marker prints.
    // This is what proves the first leg's silence is about the OPERATOR and not
    // about the test program failing to run.
    {
        std::string src = prefix + body;
        const std::string k = "REASSIGN";
        src.replace(src.find(k), k.size(), "n := null;");
        auto r = runAotChecked(src);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_TRUE(r.stdoutText.find("IS-NULL") != std::string::npos);
    }
}

// #6346 r3: THE RULE, for both collection families and the real assignment
// spelling. `n := null;` IS a "reassignment from a nullable-typed expression"
// (§7.4 "How narrowing is invalidated"), the narrowing is dropped, and the
// following member call is refused with §7.4's diagnostic. Run for both
// families, because "for every collection family" was the issue's claim and
// this is the row that makes it a pinned measurement.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           assignment_of_null_invalidates_the_narrowing_in_both_families_spec_7_4) {
    for (const char* family : kFamilies)
        expectRejectedAfterReassignment(family, "n := null;",
                                        "n.Reverse();");
}

// #6346 r4: `??=` is the other spelling §7.4 names ("likewise after `x ??=
// expr;` with such an `expr`"), and it is the interesting one: a NULLABLE
// right-hand side does NOT re-establish the fact, because the was-null path
// leaves `n` null. So `n ??= null;` must invalidate exactly as `n := null;`
// does. Both families.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           null_coalescing_assignment_of_null_invalidates_in_both_families_spec_7_4_spec_7_7) {
    for (const char* family : kFamilies)
        expectRejectedAfterReassignment(family, "n ??= null;",
                                        "n.Reverse();");
}

// #6346 r5: NOT A COLLECTION-MATTER. The issue reported the gap "for every
// collection family"; the rule it is really about is §7.4's, whose carriers are
// locals, non-`ref` parameters and `readonly` fields regardless of type. A
// value-typed carrier (`string?`) must invalidate on the same spelling, so a
// future fix that reaches only the class-typed receiver path cannot pass.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           assignment_of_null_invalidates_a_value_typed_carrier_too_spec_7_4) {
    auto r = runAotChecked(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s: string? := \"hi\";\n"
        "    Console.WriteLine(s.Length());\n"
        "    s := null;\n"
        "    Console.WriteLine(s.Length());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNarrowNeedle));
}

// #6346 r6: THE INVALIDATION IS FLOW-SENSITIVE, so a reassignment on only one
// path still drops the fact at the join — the same all-paths rule #6115/#6114/
// #6156 exercise for conditions, loop back edges and joins. `n := null;`
// inside an `if` body, with the member call after the join, for both families.
// Without this row a fix could invalidate only on an unconditional statement.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           conditional_reassignment_invalidates_at_the_join_in_both_families_spec_7_4) {
    for (const char* family : kFamilies)
        expectRejectedAfterReassignment(
            family,
            "if Console.ReadLine() <> null then n := null; end;",
            "n.Reverse();");
}

// #6346 r7: AND THROUGH A LOOP, whose body may run zero times, so the
// after-loop state is the intersection of the entry and body facts (#2118) — a
// fact the body invalidated is dropped there too. Both families.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           reassignment_in_a_loop_body_invalidates_after_the_loop_spec_7_4) {
    for (const char* family : kFamilies)
        expectRejectedAfterReassignment(
            family,
            "while Console.ReadLine() <> null do n := null; end;",
            "n.Reverse();");
}

// #6346 r8: THE STATIC-CALL SUGAR SPELLING, after the reassignment. #6276
// guards `Array.Reverse(n)` separately from `n.Reverse()` and keyed that guard
// on both collection families, so the sugar is a second code path and is
// asserted separately rather than assumed to follow the instance form.
//
// The `FAMILY` slot appears in the SUGAR as well as the receiver type, and the
// helper substitutes the same family into both. MEASURED: an earlier draft
// hardcoded `Array.Reverse(n)` while looping over both families, and the
// `OwningArray` cell then failed on a TYPE mismatch rather than on §7.4 — a
// green-looking row asserting the wrong rule. The sugar's diagnostic also opens
// differently ("the `FAMILY.Reverse(` argument is the receiver of …"), so the
// needle is the clause both spellings share, as the #6333 rows do.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           static_call_sugar_is_refused_after_the_reassignment_spec_7_4) {
    for (const char* family : kFamilies)
        expectRejectedAfterReassignment(family, "n := null;",
                                        "FAMILY.Reverse(n);");
}

// Compile one post-reassignment program for `family` whose `use` snippet is
// the remedy under test, and return the run.
static JitResult runAfterReassignment(const char* family, const std::string& use) {
    return runAot(
        "module M;\n" + withFamily(family,
            "public class Item\nend Item;\n"
            "public class Program\n"
            "  public static method Main(): int\n"
            "    var n: FAMILY<Item>? := new FAMILY<Item>();\n"
            "    n.Reverse();\n"
            "    n := null;\n"
            "    " + use + "\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"));
}

// ORACLE NOTE, applying to both rows below. `runAot`, NOT `runAotChecked`, and
// the choice is measured rather than stylistic. These are the only rows here
// that RUN a program containing `n := null;` over an `OwningArray<Item>` — and
// that program abandons the owned array by construction (the local held it;
// `:= null` dropped the only name, and nothing can dispose it afterwards).
// MEASURED under `runAotChecked`'s ASan/LSan leg: the `OwningArray` cells exit
// non-zero on the leak these rows themselves create, while the same programs
// ACCEPT and run clean through `bezelc`. The leak is the rows' construction,
// not a compiler defect, so charging it to the ASan oracle would assert nothing
// about §7.4. `runAot` runs the IDENTICAL front-end — both helpers call the same
// `Driver::compileFile` and differ only in `opts.sanitizeAddress`
// (aot_support.hpp, "ONE PIPELINE, NO DRIFT") — and the verdict under test here
// is a front-end decision taken before codegen, so the sanitizer leg adds
// nothing to these rows.

// #6346 r9: THE TWO RUNTIME-SAFE REMEDIES after the reassignment — a
// re-established narrowing and the null-conditional call — are admitted for
// BOTH families and complete normally. This is what stops a "fix" from
// over-rejecting: a change that treats a post-reassignment `n` as unusable
// outright fails here.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           null_test_and_null_conditional_are_admitted_after_the_reassignment_spec_7_4) {
    const char* const kUses[] = {
        "if n <> null then n.Reverse(); end;",  // re-narrowed by a null test
        "n?.Reverse();",                       // null-conditional call
    };
    for (const char* family : kFamilies) {
        for (const char* use : kUses) {
            auto r = runAfterReassignment(family, use);
            BEZEL_EXPECT_TRUE(r.ran);
            BEZEL_EXPECT_EQ(r.exitCode, 0);
        }
    }
}

// #6346 r10: THE FORCE-UNWRAP after the reassignment, for BOTH families. It is
// ADMITTED BY THE COMPILER — which is the whole point: `!` is §7.8's assertion,
// so §7.4 has nothing left to say about it.
//
// The two halves are asserted separately because they are separate claims, and
// MEASURED, not assumed:
//   * COMPILE: the program builds. This is the §7.4-relevant half and the one a
//     fix could break.
//   * RUN: `n` really IS null here, so `!` trips its runtime assertion and the
//     program exits non-zero (MEASURED: exit 70 for both families). That is the
//     force-unwrap behaving correctly, NOT a defect — asserting `exitCode == 0`
//     for this cell, as an earlier draft of this row did, asserted the opposite
//     of what `!` means and failed on correct behaviour.
//
// The exit code is checked as "non-zero", not against the literal 70: the exact
// code is the runtime's, and the claim under test is that `!` does not run
// cleanly over a value it knows is null.
BEZEL_TEST(reassignment_of_null_refuted_6346,
           force_unwrap_is_admitted_by_the_compiler_and_asserts_at_runtime_spec_7_4_spec_7_8) {
    for (const char* family : kFamilies) {
        auto r = runAfterReassignment(family, "n!.Reverse();");
        // The compile leg: §7.4 does not refuse the force-unwrap.
        BEZEL_EXPECT_TRUE(r.ran);
        // The run leg: `n` is null, so the assertion fires.
        BEZEL_EXPECT_TRUE(r.exitCode != 0);
    }
}