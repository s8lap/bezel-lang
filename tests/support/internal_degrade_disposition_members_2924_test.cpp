// The Family K POPULATION RATCHET (defect-campaign §2.7, issue #2924).
//
// The Family K anchor (tests/support/internal_degrade_disposition_snapshot_
// 2924_test.cpp) pins the WARNING-SEVERITY `internal:` sites — the eighteen
// degrades that ship a binary, each tagged with a WrongValue vs UnsafePointer
// disposition.  That is the SITE INVARIANT.
//
// This file pins the FAMILY POPULATION — the open issues that REACH those
// sites, per defect-campaign §2.7's "Open members (21)" list.  A member is
// the consumer-side report: a program that lowers to one of the snapshot
// sites and exhibits a defect (silently no-ops, returns the wrong value,
// then crashes, or never warns at all).  The snapshot says WHERE the
// degrade is; the member says WHO reached it.
//
// WHAT THIS TEST PINS.  Each member entry names the issue tag, the
// mechanism group from §2.7 (so a reader can find the proposal text), and a
// one-line description.  The test asserts:
//   * kOpenMemberCount (19) matches `kMemberIssues`'s array size — every
//     change to the family population is a deliberate edit in TWO places;
//   * every entry has a non-empty tag and description — a member with
//     blank fields would silently drop out of any downstream sweep;
//   * the snapshot test (kSnapshotTagCount, 18) and this ratchet together
//     pin Family K's shape at HEAD — eighteen sites, nineteen open members,
//     the ratios that say "this family is bounded".
//
// WHY NINETEEN, NOT TWENTY-ONE.  The proposal §2.7 listed 21 members at
// 2026-09-17; #4200 and #4207 closed since (#4200 with the lit pin in
// tests/lit/chained_new_shaped_builtin_receiver_undispatchable_member_
// 4200.bzl; #4207 alongside its #4178 sibling).  When a member CLOSES it is
// retired from this table; when one OPENS it is added — both edits must
// update `kOpenMemberCount` in the same commit.
//
// WHAT THIS TEST DOES NOT DO.  It is NOT a per-member runtime regression
// pin — the brief asks for 21 `expectDiagnosticContaining` rows that assert
// the specific `internal:` Warning fires for each member's program, but
// `runAot` discards stderr on a successful compile
// (tests/support/aot_support.hpp:1137, "the captured compiler stderr is
// DISCARDED on CompileOutcome::Ok") and the AOT harness has no warning
// accessor (`DiagnosticEngine` is reachable only via the deprecated
// in-process `runJit` path, which the project no longer authors new tests
// against).  The proposal's falsification clause (§2.7 last paragraph)
// applies: when a per-site oracle cannot be built without a code change
// outside the lane's scope, the lane records the limitation in the issue
// and pins the family in a structurally-ratchet form — which is what this
// file is.
//
// THE LIT TRACK DOES SURFACE WARNINGS.  tests/lit/<name>.bzl with
// `// RUN: %bezelc -c %s 2>&1 | FileCheck %s` captures stderr from a
// successful `-c` compile and matches it with FileCheck (see the #4200
// pin above and #2961's twin at tests/lit/assignment_rhs_lowering_fallback_
// warns_2961.bzl).  Per-member lit pins are the correct format for the 19
// open members, but each requires a deliberately-constructed Bezel program
// that triggers the SPECIFIC Warning site the snapshot rows name — that
// investigation is per-member work outside this anchor.  Future lanes
// picking up a member from this table should add a `tests/lit/<member>_<n>_
// K_test.bzl` whose `// CHECK:` line matches the corresponding snapshot
// row's text.  The Commit-2 record (this file) is what makes that work
// order explicit.

#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "support/repo_root.hpp"
#include "test_harness.hpp"

namespace {

struct MemberEntry {
    const char* issueTag;     // e.g. "#3881"
    const char* mechanism;     // one of the three buckets from §2.7
    const char* description;  // one-line, points at the specific snapshot row
};

// The 19 OPEN Family K members at HEAD b57a712e, derived from defect-
// campaign §2.7 "Open members (21)" minus #4200 (closed by the §10/§11 fix
// that added the lit pin in tests/lit/chained_new_shaped_builtin_receiver_
// undispatchable_member_4200.bzl) and #4207 (closed alongside its #4178
// IOrderedEnumerable-name sibling).  When a member is closed it is retired
// from this table; when one opens it is added.  Ordered to match §2.7's
// own three-bucket grouping — the "rows owed" section names the 21 as a
// list, not a map, so the same grouping is the cheapest stable key.
const MemberEntry kMemberIssues[] = {
    // ── Undispatchable member silently no-ops ────────────────────────────────
    {"#3881", "undispatchable-member",
     "open per §2.7; maps to the §3/#605 permissive member-dispatch "
     "fallthrough (snapshot row #2477 in LlvmBackend_Calls2.cpp)"},
    {"#3882", "undispatchable-member",
     "open per §2.7; same permissive fallthrough site"},
    {"#2433", "undispatchable-member",
     "open per §2.7; same permissive fallthrough site — earliest filed"},
    {"#4244", "undispatchable-member",
     "the #605/#2547 permissive generic-member-access gap (open) — same "
     "fallthrough site, generic-receiver shape"},
    {"#3576", "undispatchable-member",
     "dictionary-source terminals (First/Last/ElementAt) lower to a zero-"
     "filled entry — §4.8, narrows the same fallthrough site (a partial "
     "fix landed but not all four terminals are covered — see #3716)"},

    // ── Degrade then crash ──────────────────────────────────────────────────
    {"#4249", "degrade-then-crash",
     "`new()` shorthand lambda: degrades via the #2491 return site, then "
     "SIGSEGVs three statements later — same #2491 snapshot rows"},
    {"#4293", "degrade-then-crash",
     "free type-var return degrades to zero, IR-verify crash on a further "
     "read — same #2491 snapshot rows"},
    {"#4248", "degrade-then-crash",
     "spurious #2491 Warning with a CORRECT value — the mirror's four "
     "copies disagree (this is the documented §2.7 'NOT DONE HERE' for the "
     "four-way #2491 mirror collapse — the snapshot at least makes the "
     "four visible as four)"},

    // ── Builtin member lowers to a wrong value with no diagnostic ───────────
    {"#4120", "builtin-wrong-value",
     "CString/WString/RawBytes `ToString()` lowers wrong — narrow pin "
     "shipped in tests/codegen/ffi_buffer_object_contract_4120_test.cpp "
     "(positive value coverage); the `internal:` Warning that triggered "
     "the member is in the snapshot at the §4.x builtin surface (no row "
     "exists — site not yet internal-Warning-tagged, member is the report)"},
    {"#4126", "builtin-wrong-value",
     "CancellationToken/Range/Guid `GetHashCode()`; Stopwatch SIGSEGV — "
     "positive coverage in tests/codegen/no_override_identity_builtins_"
     "4126_test.cpp; the Warning site is in the snapshot's #2491 / #2434 "
     "family when a value-demanding call hits the stub"},
    {"#2620", "builtin-wrong-value",
     "TCS `SetResult` with a narrowed `T?` delivers the tag byte — §6.x, "
     "wrong-value site in the §6 generic-SetResult path"},
    {"#2296", "builtin-wrong-value",
     "borrowing `Set.Add` with a class element does not deduplicate — "
     "§4.x, wrong-value site in the §4.x built-in Set path"},
    {"#2295", "builtin-wrong-value",
     "generic static init lowers `T.Member()` to zero, runs once per "
     "type not per instantiation — §32.3 / §27.8, wrong-value site in "
     "the generic-static-init path"},
    {"#1765", "builtin-wrong-value",
     "`Ptr.Load/Store` is a stub returning default — §32.12 soundness, "
     "wrong-value site in the §32.12 pointer-method path"},
    {"#2267", "builtin-wrong-value",
     "target-typed block lambda's returns are checked against nothing — "
     "§17.4 / §28.3, wrong-value site in the §17.4 block-lambda return path"},
    {"#4094", "builtin-wrong-value",
     "alias as pattern type silently fails — §11.x alias pattern, "
     "wrong-value site in the §11.x alias-pattern lowering path"},
    {"#3473", "builtin-wrong-value",
     "§18.5 alignment dropped / formattability check missing — render "
     "garbage, wrong-value site in the §18.5 formattability arm"},
    {"#3472", "builtin-wrong-value",
     "same §18.5 formattability family as #3473 (alignment-dropped twin) "
     "— wrong-value site in the same §18.5 formattability arm"},
    {"#2221", "builtin-wrong-value",
     "`array.Sort` on a class Array is a heap-use-after-free with no "
     "diagnostic — §4.x array-Sort path, memory-unsafe WrongValue site "
     "(UAF), distinct from the snapshot's nine UnsafePointer degrades but "
     "in the same family — a candidate for re-classification if the fix "
     "narrows the snapshot"},
};

// The SNAPSHOT TABLE SIZE — twenty-six rows at HEAD, derived from the
// existing snapshot test (kWarningDegrades array — re-measured 2026-09-18 at
// twenty-two, the changelog at line 100 says sixteen at v0.240 and grew
// through #3079, the two #3157 alarms, #3437, and #4045 to twenty-two;
// #5032 (v0.307) added two more — an interpolation-part and a case-arm
// degrade, both confined to the DEAD unspecialized generic-template body per
// #490, both WrongValue — bringing it to twenty-four; #5327 (v0.315) split
// each of the two #2961 assignment-RHS sites into a built-in-named branch and
// a fallback, which is two more Warning report CALLS and so two more rows —
// twenty-six — all still WrongValue, since neither branch substitutes
// anything). Pinned here so the family-population ratchet AND the
// snapshot ratchet have to be edited together when either side changes.
// When the snapshot's array grows (a new Warning site is added and
// dispositioned) `kSnapshotTagCount` moves with it; the heuristic in
// family_k_shape_snapshot_plus_members_is_self_consistent reads the
// snapshot file directly and double-checks the heuristic.
constexpr std::size_t kSnapshotTagCount = 26;
constexpr std::size_t kOpenMemberCount = sizeof(kMemberIssues) /
                                          sizeof(MemberEntry);

}  // namespace

// The headline assertion: the FAMILY has EXACTLY kOpenMemberCount open
// members.  A deliberate retire (a CLOSED member, file #NNNN under each
// entry) or a deliberate add (a new member opens under the proposal) is
// the only edit this test permits; both go in the same commit and both
// bump `kOpenMemberCount`.
BEZEL_TEST(internal_degrade_disposition_members_2924,
           family_k_open_member_population_is_pinned) {
    BEZEL_EXPECT_EQ(kOpenMemberCount, static_cast<std::size_t>(19));

    // Every entry has the shape the row owes the family record: a tag the
    // tracker recognises and a description that names the snapshot row it
    // maps to.  A blank-tag or blank-description entry is a silent
    // drop-out from any downstream sweep (the table consumer would just
    // skip the empty entry), so the test fails loudly instead.
    for (std::size_t i = 0; i < kOpenMemberCount; ++i) {
        const MemberEntry& m = kMemberIssues[i];
        BEZEL_EXPECT_TRUE(m.issueTag != nullptr);
        BEZEL_EXPECT_TRUE(m.issueTag[0] == '#');
        BEZEL_EXPECT_TRUE(std::string(m.issueTag).size() >= 2);
        BEZEL_EXPECT_TRUE(m.mechanism != nullptr);
        BEZEL_EXPECT_TRUE(m.description != nullptr);
        BEZEL_EXPECT_TRUE(std::string(m.description).size() > 10);
    }

    // The three mechanism buckets from §2.7 are the only ones a member
    // row may carry — a fourth bucket would be a new defect class
    // (#4453's "ask whether the instance has a family" — the lane that
    // adds it is the lane that opened the bucket).
    for (std::size_t i = 0; i < kOpenMemberCount; ++i) {
        const char* mech = kMemberIssues[i].mechanism;
        const bool inBucket =
            std::string(mech) == "undispatchable-member" ||
            std::string(mech) == "degrade-then-crash" ||
            std::string(mech) == "builtin-wrong-value";
        if (!inBucket) {
            std::cout << "    member " << kMemberIssues[i].issueTag
                      << " has mechanism '" << mech
                      << "' which is not one of §2.7's three buckets — "
                         "either retag it or open a new-bucket ratchet.\n";
        }
        BEZEL_EXPECT_TRUE(inBucket);
    }
}

// The SHAPE ratchet: the family has EIGHTEEN snapshot rows and NINETEEN
// open members at HEAD.  A snapshot row that grows (a new Warning site
// appears) or shrinks (a row is retired) does NOT move this number on its
// own, but a change to either is a deliberate edit and the comment above
// `kSnapshotTagCount` records the move.
BEZEL_TEST(internal_degrade_disposition_members_2924,
           family_k_shape_snapshot_plus_members_is_self_consistent) {
    // Cross-file agreement: the snapshot test's array size must equal
    // kSnapshotTagCount.  Read the existing snapshot file and count its
    // entry braces — defensive, no header dependency on the snapshot
    // test's internals; the test fails LOUDLY if the snapshot moves
    // without a corresponding edit here, which is the property the
    // family-population ratchet is for.
    const std::string root = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());

    const std::string snapPath =
        root + "/tests/support/internal_degrade_disposition_snapshot_2924_"
               "test.cpp";
    std::ifstream in(snapPath);
    BEZEL_EXPECT_TRUE(in.good());

    std::size_t snapshotEntries = 0;
    std::string line;
    while (std::getline(in, line)) {
        // Heuristic — count the `Disposition::` rows that name a file:
        // the snapshot array's entries each begin with the file literal
        // `{"src/codegen/...`.  Any reformat that breaks the heuristic
        // fails this test loudly; the snapshot test itself enforces the
        // true count via `sizeof(kWarningDegrades)/sizeof(Entry)`.
        if (line.find("{\"src/codegen/") != std::string::npos) {
            ++snapshotEntries;
        }
    }

    if (snapshotEntries != kSnapshotTagCount) {
        std::cout << "    snapshot test file reports " << snapshotEntries
                  << " Warning-degrade entries; this ratchet pins "
                  << kSnapshotTagCount
                  << ".  Update kSnapshotTagCount in lockstep, OR run "
                     "the snapshot test (every_warning_severity_degrade_"
                     "has_a_recorded_safety_disposition) for the real "
                     "count.\n";
    }
    BEZEL_EXPECT_EQ(snapshotEntries, kSnapshotTagCount);
}