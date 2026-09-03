// Spec-derived regression test for issue #3775 — sema did not reject an
// OWNING-classified `choice` as a `Dictionary<K, V>` value type (or any
// other §28.11 collection's element-category slot), even though spec
// §28.11's element-slot-policy table states the move-only row is "rejected
// at instantiation" for BOTH the borrowing and owning collection families,
// and §35.4 rule 1 makes an owning-classified choice one of §15.3's two
// move-only value type kinds.
//
// THE GOVERNING RULE. Spec §28.11 (heading "Collection element ownership"),
// "The element slot policy" table (v0.286):
//
//   | Element category                    | Borrowing family slots | Owning family slots |
//   | move-only value type (§15.3 —       | (rejected at           | (rejected at         |
//   |  owning inline arrays and owning-   |  instantiation)        |  instantiation)      |
//   |  classified choices, §35.4)         |                        |                      |
//
// §15.3 "Move-only value types": "The move-only value type kinds in this
// specification are two: the owning inline array ... and the owning-
// classified choice ([§35.4])." §35.4 rule 1: "A choice is owning if any
// case has a payload field marked `owned` ... or a payload field of a
// move-only value type."
//
// ISSUE'S OWN REPRO (measured on 8e14b4c, this lane's base commit, BEFORE
// this fix):
//
//   module M;
//   class Connection public id: int; public constructor(id: int) ... end;
//   choice Wrap Item(owned conn: Connection), None; end Wrap;
//   class C public static method Main(): int
//     using var d := new Dictionary<int, Wrap>();
//     d.Add(1, Wrap.Item(new Connection(7)));
//     return 0;
//   end Main; end C;
//
// `bezelc --sanitize=address` compiled and linked this with ZERO
// diagnostics; running it under LSan reported a 16 B INDIRECT leak (the
// `Connection` payload — `dictValueBoxFreer`'s 24 B box leak is #3770's
// separate, already-fixed concern) because nothing in Dictionary<K,V>'s
// codegen path ever runs a choice's active-case payload disposal.
//
// ROOT CAUSE. `Resolver::owningElementBadDesc` (TypeResolution_Checks.cpp,
// fix #2744/#2713) is the existing §14.4/§28.11 authority for "may this be
// an OWNING collection's OWNED element slot" — it rejects every choice
// there (owning or not — any value type is unconditionally bad for that
// slot). But NOTHING checked the BORROWING family's sole element slot(s) —
// Array<T>/LinkedList<T>/Set<T>/Stack<T>/Queue<T>/SortedSet<T>'s T, both of
// Dictionary<K,V>/SortedDictionary<K,V>, and an OwningDictionary<K,V>'s KEY
// slot (its VALUE slot is the OWNED slot #2744 already covers) — against
// §28.11's move-only row at all. A value-like or borrow-bearing choice is
// legitimately admitted there (the borrowing column's "copies" cell,
// #3770/#3768/#3780's fixtures); only a MOVE-ONLY element must be rejected,
// which needs a CLASSIFICATION the owning-slot check deliberately never
// asks (any choice is bad there, so it never needed to distinguish).
//
// THE FIX. `common::isMoveOnlyValueType` (common/OwnershipFacts.hpp/.cpp) —
// promoted VERBATIM from a TU-local predicate of the same name in
// sema/ExprTypecheck.cpp (fix #2912, the §8.6 boxing rejection) so the two
// checks share one decision instead of drifting (CLAUDE.md §6 / LANE_RULES
// §6: "duplicated classification predicates" is the project's dominant
// defect generator) — is now also consulted by a new deferred check,
// `Resolver::checkElementCategoryInstantiations` (modelled two-pass, on
// `checkHashContainerKeys`'s #1338/#1384 shape: a choice's classification
// needs its case payloads resolved, which may postdate the recording site,
// exactly `checkHashContainerKeys`' own rationale). Every element-category
// slot of the spec's twelve concrete collection types plus stdlib
// §4.12/§4.13's SortedDictionary/SortedSet is recorded
// (`moveOnlyElementSites_`) EXCEPT the owning family's already-checked owned
// slot, and rejected if the bound type is move-only.
//
// §6b-STYLE EXCLUSION NOTE (not a strict unification lane, but the same
// question applies): what does the promoted predicate now reach that the
// original TU-local one did not? Nothing new structurally — it is the exact
// same two-fact test (arrayOwned flag, common::classifyChoiceType) called
// from a second call site. The original call site (ExprTypecheck.cpp's §8.6
// boxing check) is UNCHANGED (it now delegates to the shared function; see
// that file's diff) and its own tests are unaffected.
//
// SCOPE DECLINED, FILED SEPARATELY (CLAUDE.md "decline-with-reasons"):
//   - `OwningBuffer<T>`/`Span<T>`/`ReadOnlySpan<T>`: stdlib §14.4 states they
//     "still reject ... move-only value types", but MEASURED on this fix,
//     `OwningBuffer<Wrap>` for an owning-classified `Wrap` still compiles
//     with zero diagnostics — a pre-existing, separate gap this lane does
//     not touch (different admission rule: these three admit value-typed T
//     in general, unlike a plain §28.11 collection). Filed as a new issue.
//   - The DIAGNOSTIC LOCATION is `0:0` for every row here. MEASURED: this is
//     NOT a regression this fix introduces — the neighboring, already-
//     shipped `owningElementBadDesc` rejection (#2744/#2713, unrelated to
//     this fix) reports at the exact same `0:0` for the exact same AST
//     position (a generic type argument's span is not populated inside a
//     `new T<Args>()`/typed-local-annotation node). Filed as a new issue
//     covering the whole family (hashContainerSites_ / owningElementSites_ /
//     moveOnlyElementSites_ all share it). Every assertion below is
//     therefore against diagnostic TEXT, never location.
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's rebase, 8e14b4c):
// a PARENT bezelc built via a separate worktree checkout of 8e14b4c compiles
// the issue's own repro with ZERO diagnostics (`exit=0`); the FIX compiler
// rejects it with the §28.11/§15.3 diagnostic asserted below.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace bezel::sema {
namespace {

// True iff an ERROR diagnostic is THIS defect's §28.11/§15.3 rejection,
// naming the collection, the slot, and the move-only kind. Deliberately not
// a bare "compile failed": `runAot` synthesizes an Error diagnostic for a
// runtime crash too (LANE_RULES §3), and both PASS 1 and PASS 2's wording
// differ slightly (see checkElementCategoryInstantiations), so this checks
// only the substrings BOTH share.
bool hasMoveOnlyElementError(const JitResult& r, const std::string& container,
                             const std::string& slot,
                             const std::string& moveOnlyKind) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("\xC2\xA7""28.11") == std::string::npos) continue;
        if (d.message.find("\xC2\xA7""15.3") == std::string::npos) continue;
        if (d.message.find("`" + container + "`") == std::string::npos) continue;
        if (d.message.find("its " + slot + " type argument") ==
            std::string::npos)
            continue;
        if (d.message.find(moveOnlyKind) == std::string::npos) continue;
        return true;
    }
    return false;
}

// Compiles, links, runs, and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// A choice with an `owned` class payload — §35.4 rule 1, owning-classified,
// hence move-only (§15.3). `Conn` is the class payload.
const std::string kOwningChoiceDecl =
    "class Conn public id: int := 0; end Conn; "
    "choice Wrap Item(owned c: Conn), None; end Wrap; ";

}  // namespace

// =============================================================================
// REJECT — Pass 1 (concrete slots). Every case asserts diagnostic TEXT.
// =============================================================================

// The issue's OWN repro: an owning-classified choice as Dictionary<K,V>'s V.
BEZEL_TEST(sema_collection_element_move_only_3775,
           dict_value_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Wrap>(); "
        "  d.Add(1, Wrap.Item(new Conn())); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "Dictionary", "V",
                                              "an owning-classified choice"));
}

// §28.11's dictionary-key clause: "an owning dictionary owns its *values*;
// its keys are value-like and classified per the borrowing column" — so a
// borrowing Dictionary<K,V>'s KEY is ALSO classified per the same move-only
// row, independent of the value slot.
BEZEL_TEST(sema_collection_element_move_only_3775,
           dict_key_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var d := new Dictionary<Wrap, int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "Dictionary", "K",
                                              "an owning-classified choice"));
}

// §28.11's collection-family list, borrowing side: Array<T>.
BEZEL_TEST(sema_collection_element_move_only_3775,
           array_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var a := new Array<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasMoveOnlyElementError(r, "Array", "T", "an owning-classified choice"));
}

// §28.11's collection-family list: LinkedList<T>.
BEZEL_TEST(sema_collection_element_move_only_3775,
           linkedlist_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var a := new LinkedList<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "LinkedList", "T",
                                              "an owning-classified choice"));
}

// §28.11's collection-family list: Stack<T>.
BEZEL_TEST(sema_collection_element_move_only_3775,
           stack_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var a := new Stack<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasMoveOnlyElementError(r, "Stack", "T", "an owning-classified choice"));
}

// §28.11's collection-family list: Queue<T>.
BEZEL_TEST(sema_collection_element_move_only_3775,
           queue_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var a := new Queue<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasMoveOnlyElementError(r, "Queue", "T", "an owning-classified choice"));
}

// Set<T>: an owning choice is ALSO not hashable (§35.7), so this site draws
// TWO diagnostics (the pre-existing #1338 hashability reject and this fix's
// §28.11 reject). Assert this fix's specifically — a coarse "compile failed"
// check would not distinguish it from the pre-existing one.
BEZEL_TEST(sema_collection_element_move_only_3775,
           set_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var a := new Set<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasMoveOnlyElementError(r, "Set", "T", "an owning-classified choice"));
}

// stdlib §4.17 — SortedDictionary<K,V>'s V slot.
BEZEL_TEST(sema_collection_element_move_only_3775,
           sorted_dictionary_value_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var d := new SortedDictionary<int, Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "SortedDictionary", "V",
                                              "an owning-classified choice"));
}

// stdlib §4.12 — SortedSet<T>.
BEZEL_TEST(sema_collection_element_move_only_3775,
           sorted_set_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var a := new SortedSet<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "SortedSet", "T",
                                              "an owning-classified choice"));
}

// OwningDictionary<K,V>'s KEY slot — the slot #2744's owned-slot check does
// NOT cover (that check targets V, the owned slot). V here is a plain class
// so the pre-existing #2744 check has no reason to fire at all; only this
// fix's K-slot check does.
BEZEL_TEST(sema_collection_element_move_only_3775,
           owning_dictionary_key_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class Holder public n: int := 0; end Holder; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<Wrap, Holder>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "OwningDictionary", "K",
                                              "an owning-classified choice"));
}

// §14.4: "Nullable class types and interfaces are admitted. The owning-type
// instantiation rule checks the underlying type, not the nullable-wrapper."
// The SAME underlying-type reading applies here — `Wrap?` is still an
// owning-classified choice underneath the nullable flag.
BEZEL_TEST(sema_collection_element_move_only_3775,
           dict_value_nullable_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Wrap?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "Dictionary", "V",
                                              "an owning-classified choice"));
}

// §15.3 names TWO move-only kinds; the owning inline array is the other one
// — this fix's classifier (common::isMoveOnlyValueType) must catch both, not
// only choices.
BEZEL_TEST(sema_collection_element_move_only_3775,
           dict_value_owning_inline_array_rejected_3775) {
    auto r = runAot(
        "module M; "
        "class Conn public id: int := 0; end Conn; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, [2 owned]Conn?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "Dictionary", "V",
                                              "an owning inline array"));
}

// =============================================================================
// REJECT — Pass 2 (a bare generic parameter of the enclosing type, re-
// checked at each instantiation — modelled on checkHashContainerKeys'
// #1384 sweep and checkOwningElementInstantiations' #2713 sweep).
// =============================================================================

// §3.6 rule 2 / §28.11, v0.286 — `Dictionary<int, T>` inside `class H<T>` is
// admitted at the declaration site (T is a placeholder there); the owning
// choice reaching it through `H<Wrap>` is caught only by the per-
// instantiation sweep.
BEZEL_TEST(sema_collection_element_move_only_3775,
           generic_bare_param_bound_to_owning_choice_rejected_3775) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class H<T> "
        "  private owned f: Dictionary<int, T> := new Dictionary<int, T>(); "
        "  public method N(): int return f.Count; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<Wrap>(); "
        "  return h.N(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasMoveOnlyElementError(r, "Dictionary", "V",
                                              "an owning-classified choice"));
}

// =============================================================================
// ACCEPT CONTROLS — a false reject is worse than the bug (LANE_RULES §7).
// =============================================================================

// §28.11's borrowing column, "value type (non-move-only)" row → "copies": a
// VALUE-LIKE choice (no owned/class payload) is not move-only and must stay
// legal — the #3770/#3768/#3780 fixtures' own shape.
BEZEL_TEST(sema_collection_element_move_only_3775,
           dict_value_valuelike_choice_accepted_runs_3775) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "choice VLC A, B(n: int); end VLC; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, VLC>(); "
        "  d.Add(1, VLC.B(3)); "
        "  return 0; "
        "end Main; end C;"));
}

// §28.11's borrowing column: a BORROW-BEARING choice (a plain, non-`owned`
// class payload) is likewise not move-only — copied by value, borrows and
// all — and must stay legal.
BEZEL_TEST(sema_collection_element_move_only_3775,
           dict_value_borrowbearing_choice_accepted_runs_3775) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "class Conn public id: int := 0; end Conn; "
        "choice BBC A, B(c: Conn); end BBC; "
        "class C public static method Main(): int "
        "  using var conn := new Conn(); "
        "  using var d := new Dictionary<int, BBC>(); "
        "  d.Add(1, BBC.B(conn)); "
        "  return 0; "
        "end Main; end C;"));
}

// §28.11: "an owning dictionary owns its *values*; its keys are value-like."
// A VALUE-LIKE choice at OwningDictionary's KEY slot is not move-only and
// must stay legal (mirrors owning_collection_choice_element_2744_test.cpp's
// `owning_dictionary_key_slot_choice_admitted_2744`, confirming this fix's
// new K-slot check does not re-open that control).
BEZEL_TEST(sema_collection_element_move_only_3775,
           owning_dictionary_key_valuelike_choice_accepted_3775) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "class Holder public n: int := 0; end Holder; "
        "choice VLC A, B(n: int); end VLC; "
        "class C public static method Main(): int "
        "  using var d: OwningDictionary<VLC, Holder> := "
        "      new OwningDictionary<VLC, Holder>(); "
        "  return d.Count; "
        "end Main; end C;"));
}

// ⚠ THE FALSE-REJECT GUARD for Pass 2, mirroring #2713's
// `uninstantiated_generic_template_admitted_2713`: inside a generic body
// that is never instantiated, T is a placeholder (Kind::Named, declaration
// == nullptr) — common::isMoveOnlyValueType must answer false for it, not
// true, or every uninstantiated `Dictionary<K,T>` field anywhere would
// falsely reject at its own declaration.
BEZEL_TEST(sema_collection_element_move_only_3775,
           generic_uninstantiated_template_accepted_3775) {
    BEZEL_EXPECT_TRUE(runs(
        7,
        "module M; "
        "class H<T> "
        "  private owned f: Dictionary<int, T> := new Dictionary<int, T>(); "
        "  public method N(): int return f.Count; end N; "
        "end H; "
        "class C public static method Main(): int return 7; end Main; end C;"));
}

// The Pass 2 sweep's ACCEPT half: a generic parameter bound to a proper
// CLASS through `H<Res>` must run cleanly.
BEZEL_TEST(sema_collection_element_move_only_3775,
           generic_bare_param_bound_to_class_accepted_runs_3775) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class H<T> where T: class "
        "  private owned f: Dictionary<int, T> := new Dictionary<int, T>(); "
        "  public method N(): int return f.Count; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<Res>(); "
        "  return h.N(); "
        "end Main; end C;"));
}

// =============================================================================
// CROSS-MODULE — an owning-classified choice IMPORTED from another module
// (§35.4's classification must resolve identically for an imported
// TypeSymbol; fix #3144 already established this for the classifier itself,
// this pins it for the new call site too).
// =============================================================================

namespace {

namespace fs = std::filesystem;

struct TempFile3775 {
    std::string path;
    explicit TempFile3775(std::string p) : path(std::move(p)) {}
    ~TempFile3775() { std::remove(path.c_str()); }
};

void writeSource3775(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compiles `libSrc` to an object, then compiles `appSrc` importing it.
// Returns {success, capturedStderr}.
std::pair<bool, std::string> compileImportedRejects3775(
    const std::string& libSrc, const std::string& appSrc) {
    auto dir = wtScratchDir("wt-3775");
    std::string stem = "bz3775_" + unique_stem("");
    TempFile3775 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3775 aObj((dir / (stem + "_a.o")).string());
    writeSource3775(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {false, "lib failed to compile"};
    }
    TempFile3775 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3775 bExe((dir / (stem + "_b")).string());
    TempFile3775 bErr((dir / (stem + "_b.err")).string());
    writeSource3775(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink =
        ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bool ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                              deps, opts).success;
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
    std::ifstream in(bErr.path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return {ok, ss.str()};
}

const char* kLib3775 =
    "module Lib3775; "
    "public class Conn3775 public id: int := 0; end Conn3775; "
    "public choice Wrap3775 Item(owned c: Conn3775), None; end Wrap3775; ";

}  // namespace

BEZEL_TEST(sema_collection_element_move_only_3775,
           imported_owning_choice_dict_value_rejected_3775) {
    auto [ok, err] = compileImportedRejects3775(
        kLib3775,
        "module App3775; import Lib3775; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Lib3775.Wrap3775>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(err.find("\xC2\xA7""28.11") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("\xC2\xA7""15.3") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("an owning-classified choice") !=
                      std::string::npos);
}

}  // namespace bezel::sema
