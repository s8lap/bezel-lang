// fix #9058 (spec §8.2 / §17.10, stdlib §5, v0.359): the two diagnostics that
// defer an unimplemented feature cited TRACKERS THAT ARE CLOSED, so a reader
// following the number landed on closed work.
//
//   1. The stored-`GroupBy` gate (stdlib §5) said "(#755)". #755 is CLOSED —
//      its resolution was to gate the stored-var shape behind this clean
//      diagnostic, NOT to implement the value model — so the underlying gap is
//      tracked by the OPEN successor #4694 ("GroupBy stored-in-a-variable is
//      still permanently gated, not implemented — follow-up to closed #755").
//
//   2. The reflection refusals (`v.GetType()` per §8.2, `typeof(T)` per
//      §17.10) said "(#487)". #487 is CLOSED — it shipped the interim
//      "reflection not yet implemented" refusal. The open successors are
//      #3819 (stdlib §2.13's `Type` member lattice, documented normatively
//      with no implementation) and #4630 (typeof/`Type` reflection still
//      unimplemented).
//
// THIS IS A TEXT FIX, NOT A BEHAVIOUR FIX. Every shape is still refused with
// the same verdict; only the tracker NUMBERS in the message change.
//
// OBSERVABILITY. The pre-fix behaviour is a clean COMPILE-TIME REJECT whose
// message names the closed tracker, so the oracle is the SPECIFIC diagnostic
// TEXT — never `failedAtCompileTimeWithRealLocation` alone (which would
// answer true for any unrelated rejection) and never `!r.ran` (a runtime
// crash also synthesizes an Error diagnostic — tests/README.md). Every row
// asserts the POSITIVE new text, so the negative (the closed number is gone)
// is never vacuous.
//
// per §8.2, §17.10, stdlib §5, v0.359

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The NEW citations (the open successors).
constexpr const char* kGroupByNew = "(#4694)";
constexpr const char* kGetTypeNew = "(\xc2\xa7""8.2, #3819, #4630)";
constexpr const char* kTypeofNew = "(\xc2\xa7""17.10, #3819, #4630)";

// The OLD citations (the closed trackers) — asserted ABSENT on every row.
constexpr const char* kGroupByOld = "(#755)";
constexpr const char* kGetTypeOld = "(\xc2\xa7""8.2, #487)";
constexpr const char* kTypeofOld = "(\xc2\xa7""17.10, #487)";

// The stable prefixes — the diagnostics themselves, so a row pins the
// refusal and not merely a number that could appear anywhere.
constexpr const char* kGroupByPrefix =
    "a GroupBy(...) result cannot be stored in a variable yet";
constexpr const char* kGetTypePrefix =
    "reflection is not yet implemented: 'GetType()'";
constexpr const char* kTypeofPrefix = "reflection is not yet implemented: 'typeof'";
constexpr const char* kCodegenGetTypePrefix =
    "`GetType()` is not yet implemented in codegen";

// True iff NO diagnostic in `r` contains `sub`. A CHECK-NOT on a vanished
// literal would otherwise be vacuous, so every row that uses this also
// asserts the POSITIVE text of the same diagnostic.
bool noDiagnosticContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return false;
    return true;
}

// The issue's own repro (its "Stored `GroupBy`" program, verbatim): a bare
// lazy `GroupBy(...)` result bound to a variable.
const char* kGroupByProgram = R"BZL(
module P;
record E(Dept: string, Salary: int);
public class Program
  public static method Main()
    var rows: Array<E> := [new E("a", 1), new E("b", 2)];
    var g := rows.GroupBy(lambda(e: E) => e.Dept);
    Console.WriteLine("x");
  end Main;
end Program;
)BZL";

// The issue's own repro (its "Reflection" program, verbatim): `x.GetType()`.
const char* kGetTypeProgram = R"BZL(
module P;
public class Program
  public static method Main()
    var x := 1;
    Console.WriteLine(x.GetType());
  end Main;
end Program;
)BZL";

// The issue's own TWIN: the same refusal through the `typeof` spelling
// ("`typeof(Color).EnumValues` gives the same text with `'typeof'`").
const char* kTypeofProgram = R"BZL(
module P;
enum Color Red, Green, Blue; end Color;
public class Program
  public static method Main()
    var vs := typeof(Color).EnumValues;
    Console.WriteLine("x");
  end Main;
end Program;
)BZL";

// The `?.` spelling of the same universal member. Sema refuses the plain
// spelling, so this one is refused in CODEGEN — and it cited the same closed
// #487, so it is repointed too (a reader must not be sent to closed work from
// either spelling).
const char* kNullCondGetTypeProgram = R"BZL(
module Conf;
public class Program
  public static method Main(): int
    using var t0 := Task.FromResult(2);
    var t: Task<int>? := t0;
    var b := t?.GetType()?.IsClass ?? false;
    Console.WriteLine(b.ToString());
    return 0;
  end Main;
end Program;
)BZL";

}  // namespace

// Row 1 — the stored-`GroupBy` gate names the OPEN successor #4694 and no
// longer the CLOSED #755.
//
// FAILS WITHOUT THE FIX: the base message reads "... is not implemented
// (#755) ..." — the positive needle (#4694) does not match and the negative
// one ((#755)) does.
BEZEL_TEST(tracker_citation_diagtext_9058,
           stored_groupby_gate_cites_the_open_successor_4694) {
    auto r = runAot(kGroupByProgram);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    // the same refusal, on the same shape
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGroupByPrefix));
    // the tracker is the OPEN successor
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGroupByNew));
    // and the CLOSED one is gone
    BEZEL_EXPECT_TRUE(noDiagnosticContains(r, kGroupByOld));
}

// Row 2 — the `GetType()` reflection refusal cites the OPEN successors
// #3819 and #4630 and no longer the CLOSED #487.
//
// FAILS WITHOUT THE FIX: the base message reads "... 'GetType()' (§8.2,
// #487); ...".
BEZEL_TEST(tracker_citation_diagtext_9058,
           gettype_refusal_cites_the_open_successors_3819_4630) {
    auto r = runAot(kGetTypeProgram);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGetTypePrefix));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGetTypeNew));
    BEZEL_EXPECT_TRUE(noDiagnosticContains(r, kGetTypeOld));
}

// Row 3 — the `typeof` twin cites the same OPEN successors (the issue names
// this spelling explicitly: "gives the same text with `'typeof'`").
//
// FAILS WITHOUT THE FIX: the base message reads "... 'typeof' (§17.10,
// #487); ...".
BEZEL_TEST(tracker_citation_diagtext_9058,
           typeof_refusal_cites_the_open_successors_3819_4630) {
    auto r = runAot(kTypeofProgram);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeofPrefix));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTypeofNew));
    BEZEL_EXPECT_TRUE(noDiagnosticContains(r, kTypeofOld));
}

// Row 4 — the `?.` spelling of the same universal member is refused in
// CODEGEN, and its refusal cited the same CLOSED #487; it now names the same
// OPEN successors, so the two spellings of one member cannot drift.
//
// FAILS WITHOUT THE FIX: the base message reads "... not yet emitted (§8.2,
// #487). This call ...".
BEZEL_TEST(tracker_citation_diagtext_9058,
           codegen_nullcond_gettype_refusal_cites_the_open_successors) {
    auto r = runAot(kNullCondGetTypeProgram);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCodegenGetTypePrefix));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGetTypeNew));
    BEZEL_EXPECT_TRUE(noDiagnosticContains(r, kGetTypeOld));
}
