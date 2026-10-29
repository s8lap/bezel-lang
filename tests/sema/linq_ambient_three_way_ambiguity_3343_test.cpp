// fix #3343 (§26.5 rule 4 / §5 / stdlib §9.6, v0.278): §5's auto-imported
// LINQ extension block over `IEnumerable<T>` is an extension candidate
// wherever a call site would otherwise consult extension lookup (§2.3 puts
// `System.Collections.Linq` on the auto-import list; §26.5 step 2's
// "current module AND all imported modules" half covers it). The
// §26.5.1 v0.278 amendment makes that an AMBIGUITY case whenever a
// user-declared extension block spelling the same §5 name is also visible:
// §26.5 rule 4's diagnostic must name every candidate, and there are now
// THREE — the user's two blocks plus the stdlib §5 block — for any program
// declaring two `IEnumerable<T>`/`IEnumerable<int>` blocks over a §5 name
// (`First`, `Where`, `Select`, …).
//
// Before this fix the lookup only saw the user's two blocks and the
// diagnostic said "2 extensions are equally specific", silently omitting the
// §5 candidate — a worse diagnostic than the spec describes, since
// omitting a candidate from the ambiguity message hides the language's
// only blocker against a "rename your block to avoid it" choice
// (§26.5.1's v0.278 amendment — `narrow an import` is not available against
// the auto-imported block).
//
// Axis grid (per the lane SOP):
//   * member name: First | Last | Single | ElementAt | Where | Select |
//                  Count | Contains | ToArray | Reverse — the ten §5
//                  names most likely to collide in real user code (the
//                  full §5 list is 42 names; the spec's own worked
//                  example uses `First`, which is also Array<T>-unblocked
//                  per #3342).
//   * arity axis:  0 | 1 — the §5 overloads a user block is most likely
//                  to shadow at the same arity (0 = `xs.First()`, 1 =
//                  `xs.First(pred)`).
//   * receiver type: Array<int> | OwningArray<int> | Set<int> — every
//                    receiver the §5 block applies to via builtin
//                    interface ancestry.
//   * control axis: a non-§5 name (`Pick`) remains a two-way tie (the
//                   stdlib block has no `Pick` overload, so the §5
//                   candidate is not admitted — narrows the fix's
//                   surface to name-matched collisions only).
//   * control axis: a single user block + §5 is a TWO-way tie naming
//                   BOTH arms — the diagnostic must include the §5
//                   candidate. (Pre-fix: 1-way, the user block's
//                   resolution — a wrong answer, because the §5
//                   candidate was hidden from the diagnostic despite
//                   being a candidate per §26.5 rule 2.)
//   * remedy axis: the diagnostic must NOT offer "narrow an import"
//                  against the §5 candidate (per §26.5.1 v0.278, §2.3
//                  no-opt-out, §26.1 nameless). The user's only move is
//                  renaming the user block (or hoisting to a helper).

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
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

// Locate the §26.5 rule 4 ambiguity diagnostic. Pre-#3343 reports a two-way
// tie ("2 extensions are equally specific"), post-#3343 reports a three-way
// tie ("3 extensions are equally specific") for two user blocks + §5.
bool findAmbiguity(const bezel::test::SemaResult& r,
                   std::string* outMsg = nullptr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("ambiguous extension member") == std::string::npos)
            continue;
        if (d.message.find("§26.5 rule 4") == std::string::npos) continue;
        if (outMsg) *outMsg = d.message;
        return true;
    }
    return false;
}

// Counts the candidate lines in a rule-4 diagnostic. Each line of the form
// "  extension … — module …, …" is one candidate; "remedies:" marks the
// end.
int countCandidateLines(const std::string& msg) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = msg.find("\n  extension ", pos)) != std::string::npos) {
        ++n;
        ++pos;
    }
    return n;
}

}  // namespace

// ============================================================================
// The spec's own worked example (shape) — two `IEnumerable<T>` /
// `IEnumerable<int>` blocks over `First`, with an `Array<int>` receiver.
// Before #3343: 2-way. After: 3-way (both user blocks + §5).
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           array_first_zero_arity_three_way_ambiguous) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method First(): int => 100; end; "
        "extension IEnumerable<int> public method First(): int => 200; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.First(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    BEZEL_EXPECT_TRUE(msg.find("3 extensions are equally specific")
                      != std::string::npos);
    // The diagnostic must name all three candidates: the user's two blocks
    // AND the stdlib §5 LINQ block.
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 3);
    BEZEL_EXPECT_TRUE(msg.find("extension IEnumerable<T>") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("extension IEnumerable<int>") != std::string::npos);
    // The §5 candidate's defining module is `System.Collections.Linq`
    // (auto-imported per §2.3), NOT the current module `M`.
    BEZEL_EXPECT_TRUE(msg.find("module `System.Collections.Linq`")
                      != std::string::npos);
    // Per §26.5.1 v0.278 / §2.3: the stdlib §5 candidate is not removable;
    // the remedy list MUST drop "narrow an import".
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") == std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("hoist the intended behavior")
                      != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("distinct names") != std::string::npos);
}

// ============================================================================
// The predicate-arity variant of the same shape. §5 declares `First(pred)`
// over `IEnumerable<T>` (stdlib §5), so the 1-arg call also
// admits a §5 candidate.
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           array_first_pred_three_way_ambiguous) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method First(pred: (int) -> bool): int => 100; end; "
        "extension IEnumerable<int> public method First(pred: (int) -> bool): int => 200; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.First(lambda(x: int): bool => x = 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    BEZEL_EXPECT_TRUE(msg.find("3 extensions are equally specific")
                      != std::string::npos);
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 3);
    BEZEL_EXPECT_TRUE(msg.find("module `System.Collections.Linq`")
                      != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") == std::string::npos);
}

// ============================================================================
// Control axis: a non-§5 name (`Pick`) is unaffected — the stdlib §5 block
// has no `Pick` overload, so the §5 candidate is NOT admitted and the
// diagnostic is still a clean two-way tie.
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           pick_two_user_blocks_only_no_lin_q) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method Pick(): int => 100; end; "
        "extension IEnumerable<int> public method Pick(): int => 200; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    BEZEL_EXPECT_TRUE(msg.find("2 extensions are equally specific")
                      != std::string::npos);
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 2);
    // The stdlib module must NOT be named — `Pick` is not in §5.
    BEZEL_EXPECT_TRUE(msg.find("System.Collections.Linq") == std::string::npos);
    // No §5 candidate present → "narrow an import" IS a valid remedy.
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") != std::string::npos);
}

// ============================================================================
// A second §5 name (Where — the canonical LINQ operator and the name every
// user-declared block has reached for since v0.195). Same three-way pattern.
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           where_two_user_blocks_three_way_ambiguous) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method Where(pred: (T) -> bool): IEnumerable<T> => this; end; "
        "extension IEnumerable<int> public method Where(pred: (int) -> bool): IEnumerable<int> => this; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  using var q := xs.Where(lambda(x: int): bool => x > 0); "
        "  return q.Count(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    BEZEL_EXPECT_TRUE(msg.find("3 extensions are equally specific")
                      != std::string::npos);
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 3);
    BEZEL_EXPECT_TRUE(msg.find("module `System.Collections.Linq`")
                      != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") == std::string::npos);
}

// ============================================================================
// One user block + §5 = TWO-way tie. Pre-#3343, the lookup returned the
// single user block (no ambiguity, no diagnostic) because §5 was not in
// the candidate vector. Post-#3343 the §5 candidate IS in the vector and
// the tie surfaces — the user block + §5 substitute to identical heads
// for an `Array<int>` receiver (`IEnumerable<int>` vs `IEnumerable<int>`).
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           one_user_block_vs_lin_q_two_way_ambiguous) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method First(): int => 100; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.First(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    BEZEL_EXPECT_TRUE(msg.find("2 extensions are equally specific")
                      != std::string::npos);
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 2);
    BEZEL_EXPECT_TRUE(msg.find("module `System.Collections.Linq`")
                      != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("module `M`") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") == std::string::npos);
}

// ============================================================================
// A name that is in §5 but at an arity §5 does NOT support (e.g. a 5-arg
// `First`) → §5 candidate not admitted (the stdlibLinqMemberMatches arity
// filter narrows the §5 side). The user's two blocks still tie → 2-way.
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           first_at_unmatched_arity_no_lin_q) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> "
        "  public method First(a: int, b: int, c: int, d: int, e: int): int => 100; "
        "end; "
        "extension IEnumerable<int> "
        "  public method First(a: int, b: int, c: int, d: int, e: int): int => 200; "
        "end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.First(1, 2, 3, 4, 5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    // §5 has no 5-arg `First`, so the §5 candidate is NOT admitted.
    BEZEL_EXPECT_TRUE(msg.find("2 extensions are equally specific")
                      != std::string::npos);
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 2);
    BEZEL_EXPECT_TRUE(msg.find("System.Collections.Linq") == std::string::npos);
    // No §5 candidate → "narrow an import" IS a valid remedy.
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") != std::string::npos);
}

// ============================================================================
// §5's constrained block: `extension IEnumerable<T> where T: hashable`
// declares `Contains(value: T)` (stdlib §5 second block).
// The §5 candidate's `stdHead` / `stdWhere` distinguishes the constrained
// block from the main one — when a user block shadows `Contains`, the
// diagnostic names the §5 candidate with its `where` clause spelled out
// (the "declared head including any where clause" wording §26.5 rule 4
// requires — applied uniformly to stdlib and user candidates).
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           contains_block_names_lin_q_constrained_block) {
    auto r = parseSemaAll(
        "module M; "
        "extension IEnumerable<T> public method Contains(value: T): bool => false; end; "
        "extension IEnumerable<int> public method Contains(value: int): bool => false; end; "
        "class C public static method Main(): int "
        "  return 0; "
        "end Main; "
        // `IEnumerable<int>` declares only `GetEnumerator` (stdlib, System.Collections), so
        // no instance `Contains` commits resolution (§26.5 rule 1, v0.350):
        // an `Array<int>` receiver would, since `Array<T>` declares it.
        "public static method F(xs: IEnumerable<int>): int "
        "  if xs.Contains(5) then return 1; end; "
        "  return 0; "
        "end F; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    std::string msg;
    BEZEL_EXPECT_TRUE(findAmbiguity(r, &msg));
    BEZEL_EXPECT_TRUE(msg.find("3 extensions are equally specific")
                      != std::string::npos);
    BEZEL_EXPECT_EQ(countCandidateLines(msg), 3);
    BEZEL_EXPECT_TRUE(msg.find("System.Collections.Linq") != std::string::npos);
    // The §5 candidate's site line is the auto-import annotation, not a
    // user-source span — the fix must spell it for the reader to see why
    // the block cannot be removed (the rationale, not the rule alone).
    BEZEL_EXPECT_TRUE(msg.find("auto-imported via §2.3") != std::string::npos);
    // The §5 candidate for `Contains` lives in the `where T: hashable`
    // constrained block (stdlib §5 second block). The rule-4
    // diagnostic must spell the where clause as part of the declared head —
    // "declared head including any where clause" (§26.5.1 v0.278) applies
    // uniformly to stdlib and user candidates.
    BEZEL_EXPECT_TRUE(msg.find("where T: hashable") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("narrow the import") == std::string::npos);
}
// ============================================================================
// Companion to the row above: with the SAME two user `Contains` extensions,
// an `Array<int>` receiver commits to `Array<T>.Contains` (stdlib §4.4
// declares it), so extension blocks are never consulted for the name and no
// ambiguity arises (§26.5 rule 1, v0.350). The call compiles and prints the
// instance result (`true` for a present element), not the extensions' `false`.
// ============================================================================
BEZEL_TEST(sema_lin_q_ambient_three_way_ambiguity_3343,
           contains_on_array_receiver_commits_to_instance_member) {
    auto r = bezel::test::runAot(
        "module M;\n"
        "extension IEnumerable<T> public method Contains(value: T): bool => false; end;\n"
        "extension IEnumerable<int> public method Contains(value: int): bool => false; end;\n"
        "class C public static method Main(): int\n"
        "  var xs: Array<int> := [1, 2, 3];\n"
        "  Console.WriteLine(xs.Contains(2));\n"
        "  return 0;\n"
        "end Main; end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}
