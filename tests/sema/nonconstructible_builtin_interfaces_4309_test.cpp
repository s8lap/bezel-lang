// fix #4309 (§14.2 "Constraints" / §17.11 "The `default` operator", v0.298)
// — send-back on `fix #4294`.
//
// `fix #4294` gave `IGrouping<TKey,T>` its own two-line branch in BOTH
// `Resolver::argSatisfiesNew` and `typeHasNoDefaultWalk`
// (TypeResolution_Checks.cpp) because it is a null-declaration builtin
// INTERFACE — no user `TypeSymbol`, so it never reaches the TypeSymbol-
// backed `d->kind == ast::TypeDeclKind::Interface` arm those two functions
// already apply, unconditionally, to a REAL interface. That fix's own
// commit message stated the argument in FULLY GENERAL terms: "interfaces
// are unconditionally non-defaultable/non-constructible regardless of
// `TKey`/`T` … no recursion, no `genericArgs` inspection" — a statement
// that is true of every null-declaration builtin interface, not only
// `IGrouping`.
//
// A review measured that argument had not been carried through: on
// `fix #4294`'s own shipped binary (parent `e0182bc`), the identical
// silent-accept symptom reproduces on every OTHER null-declaration builtin
// interface:
//
//   default(IEnumerable<int>)          -> compile 0, run 0   (silent accept)
//   default(IDisposable)               -> compile 0, run 0
//   default(ICollection<int>)          -> compile 0
//   default(IEvent<int>)               -> compile 0
//   default(IList<int>)                -> compile 0
//   default(IOrderedEnumerable<int>)   -> compile 0
//   default(IReadOnlyList<int>)        -> compile 0
//   HolderX<IEnumerable<int>>.Make() where T:new()        -> compile 0
//   HolderY<IDisposable>.Make() where T:new()             -> compile 0
//   HolderZ<IOrderedEnumerable<int>>.Make() where T:new() -> compile 0
//
// and confirmed, by code trace, that the remaining names reach the exact
// same unconditional `return false;` (`argSatisfiesNew`'s null-declaration
// dispatch checking `head` against only the six-name Span/ExceptionInfo
// list, `"IGrouping"`, and `"DictionaryEntry"`) and `return true;`
// (`typeHasNoDefaultWalk`'s Named-arm null-declaration branch, same three
// checks) — anything else falls through to "defer" (= constructible /
// defaultable).
//
// Quoted in full, the two spec facts this file pins for EVERY name below
// (the same two `fix #4294` already pinned for `IGrouping` alone):
//
//   §17.11's "defaultable" glossary entry: "Not defaultable: non-nullable
//   reference types...". Every interface is a non-nullable reference type.
//
//   §14.2: "Interface substitutions never satisfy `new()` — no constructor
//   exists."
//
// THE POPULATION (25 names) — derived, not re-typed: every `I*` head in
// `Resolver::builtinTypeArity`'s table (TypeResolution_Names.cpp) EXCEPT
// the seven `SemaContext::isSynthesizedBuiltinInterfaceName` names
// (IComparable, IEquatable, IEqualityComparer, IReadOnlySpan, INumber,
// ISteppable, IParsable — these already get a REAL synthesized
// `TypeSymbol` and reach the TypeSymbol-backed Interface arm directly, so
// they were never affected) and `IGrouping` itself (kept in its own
// separate branch by `fix #4294`'s deliberate judgement call — not folded
// in here, see that fix's commit and TypeResolution_Checks.cpp's own
// comment beside the shared array this file asserts against):
//
//   arity-0: IDisposable, IAsyncDisposable, ISerializable, IDeserializable,
//   ISerializer, IDeserializer, IFormattable
//
//   arity-1: IEnumerable, IEnumerator, IOrderedEnumerable, IAsyncEnumerable,
//   IAsyncEnumerator, IOwningEnumerable, IOwningEnumerator,
//   IOwningAsyncEnumerable, IOwningAsyncEnumerator, IReadOnlyCollection,
//   IReadOnlyList, ICollection, IList, IOwningCollection, IOwningList,
//   IEvent, IAsyncEvent, ISharedEvent
//
// WHY THIS IS SAFE AS A CLOSED SET, UNLIKE A SHAPE-GUESSED PREDICATE: the
// population is closed, enumerable, and UNIFORMLY non-constructible — §14.2
// is unconditional for an interface, with no per-type fact (like
// `DictionaryEntry`'s per-field, instantiation-dependent defaultability, or
// `DrainedEntry`'s declared-constraint gap, issue #4305, both intentionally
// NOT folded in here) that could make one member of this set an exception.
//
// FIXED, TypeResolution_Checks.cpp, ONE shared closed-set helper consulted
// by BOTH existing call sites (not two independent copies of a 25-name
// list — see `kNonConstructibleBuiltinInterfaceNames`'s own comment,
// immediately above `Resolver::argSatisfiesNew`, for the full derivation
// and why `common::BuiltinRegistry.cpp`'s superficially-similar
// `kBuiltinInterfaceHeads` (22 names) was NOT reused: that table is
// validated for a DIFFERENT question — "whose runtime value may be a USER
// class" (a runtime-representation/disposal classification, the exact
// complement of `isVtableLessBuiltinClassName`) — and its own membership
// proves the mismatch: it excludes `IDisposable`/`IAsyncDisposable`, lacks
// `IOrderedEnumerable`/`IFormattable` entirely, and includes `IGrouping`
// (deliberately excluded here). Agreeing on 18 of 22 names is not the same
// question (§25 in this run's LANE_RULES) — reusing it would have silently
// dropped four names AND wrongly folded `IGrouping` back in.
//
// This file's FIRST test (`closed_set_derivation_...`) asserts the shared
// array — exposed via `bezel::sema::nonConstructibleBuiltinInterfaceNames()`
// — directly against the two authoritative tables it is derived from
// (`isBuiltinTypeName`, `SemaContext::isSynthesizedBuiltinInterfaceName`)
// and against this file's own program-generating row table, so a future
// addition to EITHER authoritative table, or any drift between the
// production array and this file's rows, fails loudly here instead of
// silently changing what §14.2/§17.11 reject.
//
// MECHANICAL FAIL-WITHOUT / PASS-WITH (this file's own parent `e0182bc`,
// clean tree, `bezelc` rebuilt directly at each step, `--area sema`):
// see the commit body for the exact "N of M" count and row names.

#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Asserts a clean, real-location §17.11 rejection of a top-level
// `default(...)` expression — mirrors
// `default_and_new_reject_builtin_record_string_fields_4235_test.cpp`'s
// own `expectDefaultRejected` helper (same message-text contract, since
// both sites route through the same §17.11 diagnostic).
void expectDefaultRejected(const std::string& program, std::string_view tag) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    if (r.ran) std::cout << "    [" << tag << "] unexpectedly ran\n";
    if (!failedAtCompileTimeWithRealLocation(r)) {
        std::cout << "    [" << tag
                  << "] did not fail at compile time with a real location\n";
    }
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool msgOk = diagnosticMessageContains(
                     r, "`default(T)` is a compile error") &&
                 diagnosticMessageContains(r, "\xC2\xA7""17.11");
    if (!msgOk) {
        std::cout << "    [" << tag
                  << "] missing expected \xC2\xA7""17.11 diagnostic text\n";
    }
    BEZEL_EXPECT_TRUE(msgOk);
}

// Asserts a clean, real-location §14.2 rejection of a `where T: new()`
// substitution — mirrors the same file's `expectWhereNewRejected` helper.
void expectWhereNewRejected(const std::string& program, std::string_view tag) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    if (r.ran) std::cout << "    [" << tag << "] unexpectedly ran\n";
    if (!failedAtCompileTimeWithRealLocation(r)) {
        std::cout << "    [" << tag
                  << "] did not fail at compile time with a real location\n";
    }
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool msgOk = diagnosticMessageContains(
                     r, "does not satisfy the constraint `where T: new()`") &&
                 diagnosticMessageContains(r, "\xC2\xA7""14.2");
    if (!msgOk) {
        std::cout << "    [" << tag
                  << "] missing expected \xC2\xA7""14.2 diagnostic text\n";
    }
    BEZEL_EXPECT_TRUE(msgOk);
}

struct InterfaceRow {
    std::string_view name;
    int arity;  // 0 or 1 — matches builtinTypeArity's own grouping (see the
                // header comment above and TypeResolution_Checks.cpp's
                // `kNonConstructibleBuiltinInterfaceNames`).
};

// The SAME 25 names as `kNonConstructibleBuiltinInterfaceNames`
// (TypeResolution_Checks.cpp) — the membership-assertion test below proves
// this row table and the production array name exactly the same set,
// bidirectionally, so this is not a silently-driftable third copy.
constexpr std::array<InterfaceRow, 25> kRows = {{
    {"IDisposable", 0},
    {"IAsyncDisposable", 0},
    {"ISerializable", 0},
    {"IDeserializable", 0},
    {"ISerializer", 0},
    {"IDeserializer", 0},
    {"IFormattable", 0},
    {"IEnumerable", 1},
    {"IEnumerator", 1},
    {"IOrderedEnumerable", 1},
    {"IAsyncEnumerable", 1},
    {"IAsyncEnumerator", 1},
    {"IOwningEnumerable", 1},
    {"IOwningEnumerator", 1},
    {"IOwningAsyncEnumerable", 1},
    {"IOwningAsyncEnumerator", 1},
    {"IReadOnlyCollection", 1},
    {"IReadOnlyList", 1},
    {"ICollection", 1},
    {"IList", 1},
    {"IOwningCollection", 1},
    {"IOwningList", 1},
    {"IEvent", 1},
    {"IAsyncEvent", 1},
    {"ISharedEvent", 1},
}};

std::string typeSpelling(const InterfaceRow& row) {
    if (row.arity == 0) return std::string(row.name);
    return std::string(row.name) + "<int>";
}

std::string defaultProgram(const InterfaceRow& row) {
    return "module M4309d; "
           "class Prog4309d "
           "  public static method Main(): int "
           "    var x := default(" + typeSpelling(row) + "); "
           "    _ := x; "
           "    return 0; "
           "  end Main; "
           "end Prog4309d;";
}

std::string whereNewProgram(const InterfaceRow& row) {
    return "module M4309n; "
           "class Holder4309n<T> where T: new() "
           "  public static method Make(): owned T => new T(); "
           "end Holder4309n; "
           "class Prog4309n "
           "  public static method Main(): int "
           "    var b := Holder4309n<" + typeSpelling(row) + ">.Make(); "
           "    _ := b; "
           "    return 0; "
           "  end Main; "
           "end Prog4309n;";
}

}  // namespace

// ── MEMBERSHIP ASSERTION (LANE_RULES §25 / #4309's own "derive, then
//    verify" instruction): the shared production array, consulted by both
//    `Resolver::argSatisfiesNew` and `typeHasNoDefaultWalk`, is checked
//    directly against the two authoritative tables it was derived from —
//    NOT re-verified against a hand-typed duplicate. Also cross-checks
//    this file's own row table (used to generate every program below)
//    against that same production array, bidirectionally, so the two
//    cannot silently drift apart. ─────────────────────────────────────────
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           closed_set_derivation_matches_builtinTypeArity_minus_synthesized_excludes_igrouping) {
    const auto& prodSet = bezel::sema::nonConstructibleBuiltinInterfaceNames();
    BEZEL_EXPECT_EQ(prodSet.size(), static_cast<std::size_t>(25));

    for (std::string_view n : prodSet) {
        // (a) still a recognized builtin type name — still present in
        // `builtinTypeArity` (TypeResolution_Names.cpp).
        if (!bezel::sema::isBuiltinTypeName(n)) {
            std::cout << "    " << n << " is no longer isBuiltinTypeName\n";
        }
        BEZEL_EXPECT_TRUE(bezel::sema::isBuiltinTypeName(n));
        // (b) still ABSENT from the seven-name synthesized-interface set
        // (those already reach the TypeSymbol-backed Interface arm
        // directly and must never be name-checked here too).
        if (bezel::sema::SemaContext::isSynthesizedBuiltinInterfaceName(n)) {
            std::cout << "    " << n
                      << " is now a synthesized builtin interface name\n";
        }
        BEZEL_EXPECT_FALSE(
            bezel::sema::SemaContext::isSynthesizedBuiltinInterfaceName(n));
        // (c) IGrouping stays in its own separate `fix #4294` branch — a
        // regression pin for the deliberate judgement call not to fold it
        // in (this file's own header comment, and TypeResolution_Checks.cpp).
        BEZEL_EXPECT_TRUE(n != "IGrouping");
    }

    // (d) this file's row table names EXACTLY the same 25 names, in both
    // directions — every row is in the production set, and every
    // production entry has a row.
    BEZEL_EXPECT_EQ(kRows.size(), prodSet.size());
    for (const InterfaceRow& row : kRows) {
        bool found = false;
        for (std::string_view n : prodSet) {
            if (n == row.name) { found = true; break; }
        }
        if (!found) {
            std::cout << "    row " << row.name
                      << " is not in the production array\n";
        }
        BEZEL_EXPECT_TRUE(found);
    }
    for (std::string_view n : prodSet) {
        bool found = false;
        for (const InterfaceRow& row : kRows) {
            if (row.name == n) { found = true; break; }
        }
        if (!found) {
            std::cout << "    production entry " << n
                      << " has no row in this test file\n";
        }
        BEZEL_EXPECT_TRUE(found);
    }
}

// ── TABLE-DRIVEN REJECT SWEEP: default(...) ──────────────────────────────
// One row per name in `kRows` (== the production 25-name set, proven above).
// A failure is reported with the offending name so a regression is
// localizable to a single line, mirroring
// `builtin_member_population_2998_test.cpp`'s accumulate-then-report shape
// (this harness has no SCOPED_TRACE).
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_every_name_in_the_closed_set_spec_17_11) {
    std::vector<std::string_view> failed;
    for (const InterfaceRow& row : kRows) {
        auto r = runAot(defaultProgram(row));
        bool ok = !r.ran && failedAtCompileTimeWithRealLocation(r) &&
                  diagnosticMessageContains(
                      r, "`default(T)` is a compile error") &&
                  diagnosticMessageContains(r, "\xC2\xA7""17.11");
        if (!ok) failed.push_back(row.name);
    }
    if (!failed.empty()) {
        std::cout << "    default(...) NOT rejected for " << failed.size()
                  << " of " << kRows.size() << " names:\n";
        for (auto n : failed) std::cout << "      - " << n << "\n";
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── TABLE-DRIVEN REJECT SWEEP: where T: new() ────────────────────────────
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           where_T_new_rejects_every_name_in_the_closed_set_spec_14_2) {
    std::vector<std::string_view> failed;
    for (const InterfaceRow& row : kRows) {
        auto r = runAot(whereNewProgram(row));
        bool ok = !r.ran && failedAtCompileTimeWithRealLocation(r) &&
                  diagnosticMessageContains(
                      r, "does not satisfy the constraint `where T: new()`") &&
                  diagnosticMessageContains(r, "\xC2\xA7""14.2");
        if (!ok) failed.push_back(row.name);
    }
    if (!failed.empty()) {
        std::cout << "    where T: new() NOT rejected for " << failed.size()
                  << " of " << kRows.size() << " names:\n";
        for (auto n : failed) std::cout << "      - " << n << "\n";
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── INDIVIDUALLY-NAMED ROWS for the shapes the issue and review measured
//    directly (LANE_RULES: "25 rows is unreasonable" -> named rows for the
//    measured shapes, plus the table-driven sweep above for the rest). ───

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_ienumerable_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"IEnumerable", 1}), "IEnumerable");
}
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           where_T_new_rejects_ienumerable_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"IEnumerable", 1}), "IEnumerable");
}

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_idisposable_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"IDisposable", 0}), "IDisposable");
}
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           where_T_new_rejects_idisposable_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"IDisposable", 0}), "IDisposable");
}

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_iorderedenumerable_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"IOrderedEnumerable", 1}),
                          "IOrderedEnumerable");
}
BEZEL_TEST(
    nonconstructible_builtin_interfaces_4309,
    where_T_new_rejects_iorderedenumerable_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"IOrderedEnumerable", 1}),
                           "IOrderedEnumerable");
}

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_icollection_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"ICollection", 1}), "ICollection");
}
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           where_T_new_rejects_icollection_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"ICollection", 1}), "ICollection");
}

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_ilist_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"IList", 1}), "IList");
}
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           where_T_new_rejects_ilist_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"IList", 1}), "IList");
}

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_ievent_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"IEvent", 1}), "IEvent");
}
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           where_T_new_rejects_ievent_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"IEvent", 1}), "IEvent");
}

BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           default_operator_rejects_ireadonlylist_top_level_spec_17_11) {
    expectDefaultRejected(defaultProgram({"IReadOnlyList", 1}),
                          "IReadOnlyList");
}
BEZEL_TEST(
    nonconstructible_builtin_interfaces_4309,
    where_T_new_rejects_ireadonlylist_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(whereNewProgram({"IReadOnlyList", 1}),
                           "IReadOnlyList");
}

// ── ACCEPT-SIDE CONTROLS, REQUIRED (LANE_RULES §15): rejecting 25 names
//    that real programs actually USE demands proof that legitimate use of
//    those same names — never through `default(...)`/`new()` — still
//    compiles AND computes the right value. ───────────────────────────────

// A REAL `IEnumerable<int>` obtained from a live `Array<int>` (not a
// zero-initialized default) bound to an explicit `IEnumerable<int>` local
// and enumerated — the same shape
// `array_ienumerable_view_backing_reclaim_1871_test.cpp` already pins for
// a different reason (view/backing ownership); restated here as this
// fix's own control. 1+2+3 = 6.
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           real_ienumerable_from_array_forin_accept_control) {
    auto r = runAot(
        "module M4309acceptE; "
        "class Prog4309acceptE "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var v: IEnumerable<int> := xs; "
        "    var total: int := 0; "
        "    for var x in v do total := total + x; end; "
        "    return total; "
        "  end Main; "
        "end Prog4309acceptE;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// A REAL `IDisposable`-typed `using var` — every class implicitly
// implements `IDisposable` (stdlib §2.10), so a plain class with a
// `destructor` bound through an explicit `IDisposable` local must still
// dispose correctly. The destructor bumps a static counter once; exit
// code 1 proves both that it compiled AND that disposal actually ran
// (not merely `didNotCrash` — LANE_RULES §15/§30).
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           real_idisposable_using_var_accept_control) {
    auto r = runAot(
        "module M4309acceptD; "
        "class Res4309acceptD "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var r: IDisposable := new Res4309acceptD(); end; "
        "    return _n; "
        "  end Main; "
        "end Res4309acceptD;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A REAL `IGrouping` produced by `IEnumerable<T>.GroupBy` (stdlib §5) —
// `IGrouping` is NOT in this fix's 25-name set (fix #4294 already covers
// it separately), but the two branches sit right beside `IGrouping`'s own
// in both functions, so this is cheap, directly relevant insurance that
// this fix's new branches did not disturb `fix #4294`'s own production
// control. Same shape/expected value as
// `groupby_real_usage_accept_igrouping_spec_5_control` in the #4235/#4294
// suite and `kInnerForThenKey` in
// `tests/codegen/igrouping_value_model_755_test.cpp` (issue #755): xs =
// [1,2,3,4] keyed `x mod 2` -> groups key1={1,3}, key0={2,4}; total =
// (1*100+1+3) + (0*100+2+4) = 104 + 6 = 110.
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           real_igrouping_groupby_usage_accept_control) {
    auto r = runAot(
        "module M4309acceptG; "
        "import System.Collections.Linq; "
        "class Prog4309acceptG "
        "  public static method Main(): int "
        "    var xs := new Array<int>(); "
        "    xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
        "    var total := 0; "
        "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
        "      total := total + g.Key * 100; "
        "      for var el in g do total := total + el; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Prog4309acceptG;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 110);
}

// A genuinely defaultable `where T: new()` substitution — the CLASS route
// (§14.2: "class types with a public parameterless constructor"), the
// route every rejected interface in this file's closed set categorically
// lacks. Proves this fix's new branches, which decide purely by NAME, did
// not widen to reject an ordinary constructible class.
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           genuinely_constructible_class_where_T_new_accept_control) {
    auto r = runAot(
        "module M4309acceptC; "
        "class Widget4309acceptC "
        "  public constructor() end constructor; "
        "end Widget4309acceptC; "
        "class Holder4309acceptC<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4309acceptC; "
        "class Prog4309acceptC "
        "  public static method Main(): int "
        "    var w := Holder4309acceptC<Widget4309acceptC>.Make(); "
        "    _ := w; "
        "    return 0; "
        "  end Main; "
        "end Prog4309acceptC;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A genuinely defaultable `where T: new()` substitution — the VALUE-TYPE
// route (§14.2's second, defaultability-based satisfaction route), both at
// `default(...)` and as a top-level `where T: new()` substitution. `Guid`
// is a null-declaration builtin exactly like every name in this file's
// closed set, but WITH a real default — the near-miss this fix's new
// branches (name-scoped, not structural) must not catch. Mirrors
// `where_T_new_accepts_guid_top_level_substitution_spec_14_2` /
// `default_operator_accepts_guid_top_level_spec_17_11_control` in the
// #4235/#4294 suite, restated here as this fix's own control.
BEZEL_TEST(nonconstructible_builtin_interfaces_4309,
           genuinely_defaultable_guid_accept_control) {
    auto r1 = runAot(
        "module M4309acceptV1; "
        "class Prog4309acceptV1 "
        "  public static method Main(): int "
        "    var x := default(Guid); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4309acceptV1;");
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.exitCode, 0);

    auto r2 = runAot(
        "module M4309acceptV2; "
        "class Holder4309acceptV2<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4309acceptV2; "
        "class Prog4309acceptV2 "
        "  public static method Main(): int "
        "    var b := Holder4309acceptV2<Guid>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end Prog4309acceptV2;");
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 0);
}
