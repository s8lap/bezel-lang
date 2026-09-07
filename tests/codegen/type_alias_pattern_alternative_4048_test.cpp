// Spec-derived tests for fix #4048 — §19.3 / §27.8 (v0.294): a `type` alias
// name used as a member of a `case`/`when` `|` pattern alternative list was
// not recognized as naming a TYPE, so the §19.3 rejection ("a type may not
// appear in a `|` pattern alternative list") never fired for it. The
// malformed pattern reached codegen with no recognizable constant value to
// lower, producing an LLVM IR VERIFIER CRASH ("Basic Block ... does not have
// terminator") instead of a clean compile-time diagnostic.
//
// This is the exact `TypeAliasSymbol`-missing-arm shape fix #4038 closed one
// call chain over (`resolveMemberAccess`'s static-member-access target-type
// resolution) — found here in `ExprTC::altPartsNameType`
// (`src/sema/ExprTypecheck.cpp`), the function `checkAltMembersConstant`
// calls to decide whether a bare or qualified alternative-list member names
// a type. Its bare-name arm dispatched on `lookupName(parts[0])`'s result
// with `dynamic_cast<TypeSymbol*>` / `dynamic_cast<NestedTypeSymbol*>` and no
// `TypeAliasSymbol` arm, so a `type Alias := SomeClass;` name fell into the
// final `else return false` — "resolves to a value/const, not a type" — and
// `checkAltMembersConstant` treated it as an ordinary (but unfoldable)
// constant-pattern member instead of rejecting it.
//
// The fix resolves a `TypeAliasSymbol` head through to its aliased type's
// TypeSymbol (`ctx_.resolvedType(aliasDecl->aliasedType)`, requiring
// `TypeRef::Kind::Named` — the same fact fix #578 and fix #4038 already
// reuse), added to BOTH the primary `lookupName`-found arm and the
// not-in-scope fallback loop over `ctx_.symbols()` (defensive symmetry; the
// fallback loop is not reached by this issue's own repro, since a
// module-level alias IS found by `lookupName`).
//
// ── ORACLE CHOICE ────────────────────────────────────────────────────────
//
// ⚠ CORRECTION (found in review, verified independently before recording
// here): the pre-fix failure is NOT uniformly a compiler crash across all
// four rejection rows below. Three rows (the class, record, and chained
// cases) pre-fix ARE an LLVM IR verifier crash. The FOURTH,
// `alias_in_per_slot_alt_rejected_not_crashed`, pre-fix instead COMPILES
// CLEAN and RUNS, deterministically returning a wrong value (`1`) — a
// silent-wrong-answer failure, not a crash, on that one row specifically.
// (Measured directly: its own FAIL-WITHOUT run shows BOTH
// `failedAtCompileTimeWithRealLocation` and the diagnostic-text assertion
// failing together — the location-based helper's first check is
// `if (r.ran) return false;`, and it returned false, so `r.ran` was true —
// where the three crash rows fail on the diagnostic-text assertion ALONE,
// with the location-based helper already passing.) This is exactly why the
// helper needed pairing with the diagnostic-text check in the first place
// (see below) — the two pre-fix failure MODES (crash vs. silent-wrong-value)
// are both closed by asserting the specific §19.3 text, even though only
// three of the four rows are actually crash-shaped.
//
// The general shape below still holds for the crash rows: a bare
// `EXPECT_FALSE(r.ran)` would pass vacuously whether the fix is present or
// not for THOSE three (a crash also leaves `ran == false`), so a bare
// `EXPECT_FALSE(r.ran)` would also pass `check_ambiguous_ran_false.py`'s
// ratchet incorrectly. Every rejection assertion below therefore uses
// `failedAtCompileTimeWithRealLocation` (`tests/support/aot_result_asserts.hpp`)
// — which explicitly EXCLUDES a crash/signal/timeout/ASan outcome — AND
// additionally checks the specific §19.3 diagnostic text, so a row can only
// pass if the compiler produced exactly the intended clean rejection,
// regardless of which pre-fix failure mode it started from.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
constexpr const char* kAltTypeMsg =
    "a type may not appear in a `|` pattern alternative list";
}  // namespace

// #4048 issue repro, verbatim (§19.3 / §27.8): two class ALIASES in a `|`
// pattern alternative list. Pre-fix: LLVM IR verifier crash ("Basic Block …
// does not have terminator"). Post-fix: clean §19.3 rejection, no crash.
BEZEL_TEST(type_alias_pattern_alternative_4048,
           alias_of_class_in_alt_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type AliasDogX := DogX; "
        "type AliasCatX := CatX; "
        "public class DogX "
        "end DogX; "
        "public class CatX "
        "end CatX; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 3; "
        "    return (case x of AliasDogX | AliasCatX => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltTypeMsg));
}

// §9.2 / §19.3 / §27.8: a RECORD alias in the same shape (records are
// TypeSymbol-based exactly like classes, so this exercises the same
// classification, not a different code path — included for the grid rather
// than because a different mechanism was suspected).
BEZEL_TEST(type_alias_pattern_alternative_4048,
           alias_of_record_in_alt_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type AliasRecA4048 := RecA4048; "
        "type AliasRecB4048 := RecB4048; "
        "public record RecA4048(V: int); "
        "public record RecB4048(V: int); "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 3; "
        "    return (case x of AliasRecA4048 | AliasRecB4048 => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltTypeMsg));
}

// §27.8 "Aliases of aliases are permitted" — a CHAINED alias (inner declared
// before outer, matching #4038's own fixed ordering; #4047 tracks the
// reverse declaration order separately and is out of scope here) must also
// be recognized as naming a type.
//
// ⚠ BOTH alternative members must be ALIASES (of two distinct classes) —
// mixing one alias member with one DIRECT (unaliased) class name would let
// the direct member's own correct rejection fire and mask whether the ALIAS
// member was recognized at all. (A first draft of this test did exactly
// that and measurably passed pre-fix for the wrong reason — caught by the
// FAIL-WITHOUT cycle below, not by inspection.)
BEZEL_TEST(type_alias_pattern_alternative_4048,
           chained_alias_in_alt_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "public class ChainTargetA4048 "
        "end ChainTargetA4048; "
        "public class ChainTargetB4048 "
        "end ChainTargetB4048; "
        "type AliasChainInnerA4048 := ChainTargetA4048; "
        "type AliasChainOuterA4048 := AliasChainInnerA4048; "
        "type AliasChainInnerB4048 := ChainTargetB4048; "
        "type AliasChainOuterB4048 := AliasChainInnerB4048; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 3; "
        "    return (case x of AliasChainOuterA4048 | AliasChainOuterB4048 "
        "                        => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltTypeMsg));
}

// §19.3 (v0.167, #1371) PER-SLOT alternative: `checkAltMembersConstant`
// recurses into a `DestructuringPattern`'s subPatterns, so an alias member
// inside a per-slot `|` alternative (`Pair(AliasOfColor, _)`) reaches the
// SAME `altPartsNameType` this fix changes. `Pair`'s first slot is typed
// `Object` so a type-shaped alternative member is syntactically reachable
// there (mirroring the direct-class per-slot shape
// `pattern_alternatives_1371_test.cpp` already covers with `Dog | Cat`).
//
// ⚠ Same both-members-must-be-aliases discipline as the chained-alias test
// above — two DISTINCT alias-of-class members, no direct class name mixed
// in.
//
// ⚠ CORRECTION (see "ORACLE CHOICE" above): unlike the other three
// rejection rows in this file, this one's PRE-FIX failure mode is NOT a
// compiler crash — it compiles clean and runs, deterministically returning
// `1` (a silent wrong value: the malformed per-slot alternative is
// misclassified as an admissible-but-unfoldable constant member and the
// arm matches unconditionally, rather than failing to lower). The test
// name is kept for consistency with its siblings (all four rows share the
// "not crashed" framing describing the POST-fix guarantee), but do not
// infer from the name that this row's pre-fix symptom was itself a crash.
BEZEL_TEST(type_alias_pattern_alternative_4048,
           alias_in_per_slot_alt_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type AliasSlotX4048 := SlotClassX4048; "
        "type AliasSlotY4048 := SlotClassY4048; "
        "public class SlotClassX4048 "
        "end SlotClassX4048; "
        "public class SlotClassY4048 "
        "end SlotClassY4048; "
        "public record PairObj4048(A: Object, B: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new PairObj4048(3, 7); "
        "    return (case p of PairObj4048(AliasSlotX4048 | AliasSlotY4048, _) "
        "                        => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltTypeMsg));
}

// ── ANTI-REGRESSION: accept side (§15) ─────────────────────────────────────
//
// The fix only ADDS a new way for `altPartsNameType` to return true (through
// an alias); it must not newly reject an alternative list whose members are
// genuine CONSTANTS (never resolving to any TypeSymbol/TypeAliasSymbol at
// all, alias-classified or not). Both control shapes below are VALUE-checked
// (not just compiles-clean), mirroring the existing accept-side lit control
// `sema_alt_1371__enum_constant_alternatives_accepted.bzl`.

// §10.6 / §19.3: enum-constant alternatives (no alias involved anywhere)
// still compile and select correctly.
BEZEL_TEST(type_alias_pattern_alternative_4048,
           enum_constant_alt_still_accepted_and_correct) {
    const std::string prog =
        "module M; "
        "enum Color4048 Red, Green, Blue; end Color4048; "
        "class C "
        "  public static method M(c: Color4048): int "
        "    return (case c of Red | Blue => 1, Green => 2); "
        "  end M; "
        "  public static method Main(): int "
        "    return M(Color4048.Red) * 10 + M(Color4048.Green); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 12);  // M(Red)=1, M(Green)=2 → 1*10+2
}

// §17.14 / §19.3: named-`const` alternatives (no alias involved) still
// compile and select correctly.
BEZEL_TEST(type_alias_pattern_alternative_4048,
           named_const_alt_still_accepted_and_correct) {
    const std::string prog =
        "module M; "
        "class K4048 "
        "  public const One: int := 1; "
        "  public const Two: int := 2; "
        "end K4048; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 2; "
        "    return (case x of K4048.One | K4048.Two => 5, _ => 6); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 5);
}

// Anti-regression: the DIRECT (non-alias) type-in-alternative rejection —
// pattern_alternatives_1371_test.cpp's own `Dog | Cat` shape — must still
// reject cleanly (unaffected by this fix's new TypeAliasSymbol arm).
BEZEL_TEST(type_alias_pattern_alternative_4048,
           direct_type_in_alt_still_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "class Animal4048 end Animal4048; "
        "class Dog4048: Animal4048 end Dog4048; "
        "class Cat4048: Animal4048 end Cat4048; "
        "class C "
        "  public static method M(a: Animal4048): int "
        "    return (case a of Dog4048 | Cat4048 => 1, _ => 0); "
        "  end M; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAltTypeMsg));
}
