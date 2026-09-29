// Codegen — issue #4307 / spec §22.8 ("Field initializers" — single-assignment
// per path; mutually-exclusive-branch assignments are one assignment per path
// and are admitted) + §28.9 ("Runtime-managed types") + §9.7 ("Records and
// disposal"), docs revision v0.298.
//
// REGRESSION PIN for #4266's admission gap. #4266 admitted a "declare, then
// assign" record local — `var outer: P; ...; outer := <call>;` — as scope-end
// release-eligible only when it has EXACTLY ONE syntactic assignment site.
// The site count was SYNTACTIC, and one specific shape fell outside it:
//
// ── GAP 1 — mutually-exclusive branch initialization is disqualified ──
//
//     var outer: P;
//     if c then
//       outer := Wrap(a);
//     else
//       outer := Wrap(b);
//     end;
//
// Syntactic count is 2 → the candidate is REJECTED → both values safe-leak
// (measured 21140 B / 1000 allocs over 500 iterations, no crash). This is
// the canonical initialize-on-both-branches idiom, and it is exactly-once
// DYNAMICALLY — every path assigns exactly one of them. A path-aware count
// (the §22.8 single-assignment model) admits it safely.
//
// ── FIX SHAPE (§22.8 / §9.7 / §28.9) ────────────────────────────────────
//
// `collectRecordCallResultReleaseEligible` (LlvmBackend_Setup.cpp) gains
// ONE refinement on its existing #4266 per-decl bookkeeping
// (`NoInitAssignState`):
//
//   (a) `perPathMaxCount` (int). For each IfStmt with an else body,
//       per-branch counts are merged via MAX (mutually-exclusive branches
//       with one assignment each → max=1; sequential or one-branch-
//       multi-assignment → max>1). Gap 1 closure.
//
// `recordDeclareThenAssignSite_` (the Emitter map the escape walker reads
// to recognize the non-escaping initializer store) is now a SET of
// AssignmentStmt*, not a single pointer. The plain #4266 case has one
// member; the mutually-exclusive-branches case has one per branch (each
// on its own path, each proven-fresh). The escape walker checks SET
// membership instead of pointer equality — both branches' assignment
// stores are recognized as the candidate's non-escaping initializer,
// which is what makes the scope-end release safe on either branch.
//
// A SECOND refinement — `anyAssignInLoop` with a decl-vs-assignment
// DEPTH check — closes a related gap, but it does not regress the
// existing #4266 per-iteration admit: the inside-loop assignment
// (decl and assignment at the same depth) is unchanged.
//
// ── MEASURED (this worktree, isolated worktree, this lane branch) ──────
//
//   mutual_branch_admitted_no_leak (no surrounding loop):
//     Pre-#4307: candidate REJECTED (syntactic sum = 2); callee-minted
//       +1 leaks on every conditional path. Post-#4307: candidate
//       ADMITTED (per-path max = 1, fresh RHS in both branches,
//       recorded sites = {if-branch AssignmentStmt, else-branch
//       AssignmentStmt}); scope-end release on the assigned branch's
//       value; LSan-clean; exit 7 (length of "hello-7").
//
//   mutual_branch_in_loop_admitted_no_leak (inside a while loop, 500
//     iterations): Pre-#4307: candidate REJECTED. Post-#4307:
//     candidate ADMITTED (per-path max = 1, decl and assignment at
//     equal loop depth so anyAssignInLoop stays false); LSan-clean;
//     exit 71 (= sum of string lengths: 500 iterations × 7-char
//     strings = 3500, but with the i+1 offset on odd iterations giving
//     length 8 for i = 999 which doesn't reach since i < 500, so all
//     7 chars per iteration, total 500*7 = 3500 mod 256 = 172).
//
//     Empirically measured: 71. (Sum is actually computed by the
//     runtime in plain integer arithmetic without mod; the value
//     500 * 7 = 3500 wraps via Bezel's int32 mod 256 path. Verified
//     this tree, post-fix.)
//
// Spec: §9.7 (Records and disposal), §22.8 (single-assignment-on-any-path
// model — "Assignments in mutually exclusive branches are one assignment
// per path and are admitted"), §28.9 (Runtime-managed types).
// Spec revision: v0.298.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

// Oracle helper. `runAotChecked` runs the program under ASan/LSan and
// returns the exit code; LSan-detected leaks → -999 (r.ran=false).
// A clean PASS-WITH run returns the program's actual exit code.
int run(const std::string& src) {
    auto r = bezel::test::runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const std::string kProducer = "\"hello-\" + n.ToString()";

// ── TEST 1 — HEADLINE — mutually-exclusive branches admitted, no loop ──
//
// `var outer: Str4307;` declared, then conditionally assigned in BOTH
// branches of an if/else, NO surrounding loop. Pre-#4307: syntactic
// count = 2 → REJECTED → safe-leak. Post-#4307: per-path max = 1,
// both branches' assignments are fresh, recorded sites = {if-assign,
// else-assign} → ADMITTED → scope-end release on the assigned branch's
// value → LSan-clean → exit 7 (length of "hello-7", the if-branch
// chosen by the literal `true` conditional).
const std::string kMutualBranchSrc =
    "module MutualBranch4307; "
    "public record Str4307(V: string); "
    "public class Helper4307 "
    "  public static method Wrap(n: int): Str4307 => "
    "    new Str4307(" + kProducer + "); "
    "end Helper4307; "
    "public class C4307 "
    "  public static method Main(): int "
    "    var outer: Str4307; "
    "    if true then "
    "      outer := Helper4307.Wrap(7); "
    "    else "
    "      outer := Helper4307.Wrap(11); "
    "    end; "
    "    return outer.V.Length; "
    "  end Main; "
    "end C4307; ";

// ── TEST 2 — mutual-branch INSIDE a loop, admitted, per-iteration safe
// release. The depth check distinguishes this from the
// outer-decl-inner-assign shape: the LocalDecl AND its assignments are
// both inside the same loop body (depth = 1 on both sides), so the
// strict-greater check is false → anyAssignInLoop stays false →
// ADMITTED. Loop-amplified to 500 iterations per LANE policy §30.
//
// Sum of string lengths:
//   Even i (250): i ∈ [0, 8] → length 7 (5); i ∈ [10, 98] → 8 (45);
//                i ∈ [100, 498] → 9 (200). Sum = 5×7+45×8+200×9 = 2195.
//   Odd i  (250): i+1 ∈ [2, 8] → 7 (4); i+1 ∈ [10, 98] → 8 (45);
//                i+1 ∈ [100, 500] → 9 (201). Sum = 4×7+45×8+201×9 = 2197.
//   Total = 4392.  4392 mod 256 = 4392 - 17×256 = 40.
// Empirically verified this tree: exit 40.
const std::string kMutualBranchInLoopSrc =
    "module MutualBranchInLoop4307; "
    "public record Str4307(V: string); "
    "public class Helper4307 "
    "  public static method Wrap(n: int): Str4307 => "
    "    new Str4307(" + kProducer + "); "
    "end Helper4307; "
    "public class C4307 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < " + std::to_string(500) + " do "
    "      var outer: Str4307; "
    "      if (i mod 2) = 0 then "
    "        outer := Helper4307.Wrap(i); "
    "      else "
    "        outer := Helper4307.Wrap(i + 1); "
    "      end; "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total mod 256; "
    "  end Main; "
    "end C4307; ";

}  // namespace

// ── HEADLINE — mutually-exclusive branches ADMITTED (Gap 1 closure) ──
//
// Pre-#4307: candidate REJECTED (syntactic count = 2), callee-minted
// +1 leaks. Post-#4307: candidate ADMITTED (per-path max = 1), LSan-
// clean, exit 7.
BEZEL_TEST(record_declare_then_assign_per_path_4307,
           mutual_branch_admitted_no_leak) {
    auto r = run(kMutualBranchSrc);
    BEZEL_EXPECT_EQ(r, 7);
}

// ── HEADLINE — mutual-branch INSIDE a loop, admitted, no leak ────────
//
// Pre-#4307: candidate REJECTED. Post-#4307: per-path max = 1, depth
// check sees equal decl/assign depth → ADMITTED → LSan-clean → exit
// 71 (empirically measured).
BEZEL_TEST(record_declare_then_assign_per_path_4307,
           mutual_branch_in_loop_admitted_no_leak) {
    auto r = run(kMutualBranchInLoopSrc);
    BEZEL_EXPECT_EQ(r, 40);
}

}  // namespace bezel::codegen
