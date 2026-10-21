// §28.1 owned-marker erasure at a VALUE substitution (spec §28.1/§28.3, v0.196;
// issue #1817, sub-task E1).
//
// §28.1 (v0.196): for a bare-type-parameter `owned` slot — an `owned T` return
// or an `owned`/`out owned` parameter — the marker's meaning is fixed per
// instantiation. At a class or interface substitution it has its ordinary
// transfer meaning; at a move-only value substitution it is admitted-but-
// redundant (marker retained); and at EVERY OTHER value or runtime-managed
// substitution it is ERASED — the substituted signature is unmarked, so the
// result/parameter is a plain value with no disposal obligation (§28.9).
//
// THE BUG (#1817 E1). producerState's CallExpr branch returned OwnershipState::
// Owned for ANY `owned`-return method, ignoring the substituted result type. So
// a call to a generic `owned T`-returning method at a value substitution
// (`Factory<int>.Make(5)`) classified its int result Owned: the caller emitted a
// spurious §28.3 auto-drop must-use warning ("owned value in `n` is implicitly
// disposed …") and a bogus scope-end dispose of a value that owns nothing.
//
// THE FIX. producerState now erases the marker when the SUBSTITUTED result type
// is a value/runtime-managed kind (a primitive incl. string, a record/enum/
// value-builtin, a non-owning inline array) — never for a still-generic bare-T
// result nor a class/interface substitution (which would leak) nor a move-only
// owning inline array (marker retained). The owned-parameter seeding path
// mirrors the same gate.
//
// FAILS-WITHOUT / PASSES-WITH. Tests 1-4 assert NO auto-drop warning for the
// value-substituted result at four value kinds (int, string, record, enum): the
// warning is present without the fix and absent with it (verified on HEAD). The
// two NEG controls guard the boundary: a still-generic `owned T` param keeps its
// obligation (its undischarged auto-drop warning still fires — abstract T is NOT
// erased), and a class substitution still transfers and disposes EXACTLY ONCE
// (runAotChecked, ASan/LSan-clean — erasing there would leak).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"  // parseAndCheckOwnership + hasAutoDropWarning
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// A generic factory whose `Make` returns its bare type parameter under `owned`.
// The caller binds the result to a plain `var` and leaves it undischarged, so an
// obligation — if wrongly attached — would surface as an auto-drop warning on
// `n`. The `Factory<T>` receiver is `using var`, so it carries no warning of its
// own (it is a genuine owned class, disposed at scope end).
std::string valueCase(const std::string& decls, const std::string& targ,
                      const std::string& arg) {
    return "module M; " + decls +
           "class Factory<T> "
           "  public method Make(owned x: T): owned T return x; end Make; "
           "end Factory; "
           "class P "
           "  public static method Main(): int "
           "    using var f := new Factory<" + targ + ">(); "
           "    var n := f.Make(" + arg + "); "
           "    return 0; "
           "  end Main; "
           "end P;";
}

}  // namespace

// ===========================================================================
// (1) VALUE substitution T=int — the marker is erased; `n` is a plain int with
// no disposal obligation, so NO §28.3 auto-drop warning. (§28.1 v0.196.)
// WITHOUT the fix: the spurious "owned value in `n` …" warning fires.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817, int_result_no_auto_drop_spec_28_1) {
    OwnershipResult r = parseAndCheckOwnership(valueCase("", "int", "5"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (2) VALUE substitution T=string (runtime-managed, §28.9) — erased, no warning.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817,
           string_result_no_auto_drop_spec_28_1) {
    OwnershipResult r =
        parseAndCheckOwnership(valueCase("", "string", "\"hi\""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (3) VALUE substitution T=record — a record is a value type (§9.7, no
// destructor / no owned fields), so the erased result carries no obligation.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817,
           record_result_no_auto_drop_spec_28_1) {
    OwnershipResult r = parseAndCheckOwnership(
        valueCase("record Point(X: int, Y: int); ", "Point", "new Point(1, 2)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (4) VALUE substitution T=enum — an enum is a value type; erased, no warning.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817,
           enum_result_no_auto_drop_spec_28_1) {
    OwnershipResult r = parseAndCheckOwnership(
        valueCase("enum Color Red, Green, Blue; end Color; ", "Color",
                  "Color.Red"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "n"));
}

// ===========================================================================
// (5) VALUE substitution runtime path — the erased int round-trips as a plain
// value and the program runs clean (no bogus dispose of an int). runAotChecked
// so any stray dispose-of-a-value would flip r.ran under ASan/LSan.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817,
           int_result_runs_clean_spec_28_1) {
    JitResult r = runAotChecked(
        "module M; "
        "class Factory<T> "
        "  public method Make(owned x: T): owned T return x; end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<int>(); "
        "    var n := f.Make(5); "
        "    return n; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);  // the erased value round-trips
}

// ===========================================================================
// NEG (6) — a still-generic bare-T `owned` PARAMETER is NOT erased: its
// obligation is real (T could be a class), so an undischarged owned param still
// draws the §28.3 auto-drop warning. This guards that the parameter-seeding gate
// does not erase at the abstract (definition-side) boundary. Erasing here would
// silently drop a class resource's obligation.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817,
           abstract_t_owned_param_keeps_obligation_spec_28_1) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Wrap<T> "
        "  public method Bad(owned x: T): int return 0; end Bad; "
        "end Wrap;");
    // The undischarged owned param `x` (T abstract) is auto-dropped with a
    // must-use warning — the obligation survives (not erased).
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// ===========================================================================
// NEG (7) — a CLASS substitution still transfers ownership. `Factory<Conn>.Make`
// hands the caller an owned Conn; a `using var` binding disposes it EXACTLY once.
// runAotChecked: erasing here would leak (destructor never runs) → r.ran false.
// ===========================================================================
BEZEL_TEST(owned_bare_t_value_erasure_1817,
           class_substitution_disposes_once_spec_28_1) {
    JitResult r = runAotChecked(
        "module M; "
        "class Conn : IDisposable "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Conn; "
        "class Factory<T> "
        "  public method Make(owned x: T): owned T return x; end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Conn>(); "
        "    using var c := f.Make(new Conn(7)); "
        "    Console.WriteLine(\"body\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);            // no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 1);  // disposed once
    // Scope-end disposal: the destructor runs after the body statement.
    const std::size_t body = r.stdoutText.find("body");
    const std::size_t disp = r.stdoutText.find("D7");
    BEZEL_EXPECT_TRUE(body != std::string::npos);
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(body < disp);
}
