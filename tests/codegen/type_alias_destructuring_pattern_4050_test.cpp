// Spec-derived tests for fix #4050 — §19.3 / §27.8 (v0.294): a `type` alias
// used as a DESTRUCTURING pattern head (`case p of AliasOfRecord(x, y) =>
// ...`) crashed with an LLVM IR verifier failure instead of either
// correctly binding the destructured fields or, for an alias-of-CLASS,
// producing the clean §19.3 "not valid on a class type" rejection.
//
// Unlike #4038/#4048/#4051 — pure sema-side missing-`TypeAliasSymbol`-arm
// fixes — this bug spans BOTH layers, discovered mid-investigation:
//
//   * SEMA has (at least) three independent name-match loops that resolve a
//     destructuring pattern's head name to a record/class TypeSymbol:
//     `ExprTC::bindNestedDestructuringNames`'s `fieldsOfRecord` lambda
//     (nested sub-patterns), the CaseExpr twin
//     (ExprTypecheck_Exprs.cpp), and the CaseStmt twin (ExprTypecheck.cpp)
//     — none recognized a `TypeAliasSymbol`.
//   * CODEGEN does NOT consume sema's resolved head — `Emitter::patternMatches`
//     (LlvmBackend_Exprs2.cpp) independently RE-RESOLVES the same head name
//     to a record TypeSymbol for its own runtime type-test and field
//     extraction, with the identical gap. Fixing sema ALONE measurably left
//     the crash reproducing (confirmed empirically during this fix's own
//     investigation, not merely inferred): sema's fix changed the resulting
//     diagnostic-vs-accept classification, but codegen still could not find
//     `recTy` for the alias-qualified head and returned `nullptr`, and the
//     crash persisted identically.
//
// Fix: both layers resolve a `TypeAliasSymbol` head through the SAME shared
// free function, `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp/.cpp)
// — reachable from sema's `ExprTC` (via a thin forwarding member) and from
// codegen's `Emitter` directly (both hold a `SemaContext& ctx_`). This is
// the "one shared resolution" seam recommended during review of #4038/#4048:
// unlike those two (sema-only), #4050 could not be fixed correctly with a
// second independent copy in codegen — the two copies would have needed to
// stay in lockstep forever, exactly the drift hazard the shared function
// closes.
//
// The fix's positive effect for a RECORD alias is exact field binding with
// the CORRECT VALUE, not a converted-to-clean-rejection outcome — a codegen
// "never-match" guard alone (the #4062 sibling finding's own recommendation,
// for a DIFFERENT codegen site) would have been the WRONG fix for this
// specific bug: it would only make alias-qualified record destructuring fail
// cleanly, when the correct behavior is for it to WORK, exactly as direct
// (non-alias) record destructuring already does.

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
constexpr const char* kClassDestructureMsg =
    "is not valid on a class type";
}  // namespace

// #4050 issue repro, verbatim (§19.3 / §27.8), EXPRESSION-position `case`:
// a record alias destructuring head must bind and compute the CORRECT
// VALUE, not crash. Pre-fix: LLVM IR verifier crash ("Basic Block … does
// not have terminator").
BEZEL_TEST(type_alias_destructuring_pattern_4050,
           caseexpr_record_alias_binds_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasPointX4050 := PointX4050; "
        "public record PointX4050(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Object := new PointX4050(3, 4); "
        "    return (case p of AliasPointX4050(x, y) => x + y, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

// Same shape, STATEMENT-position `case` (the CaseStmt twin) — measured
// separately as a DIFFERENT pre-fix symptom: this shape compiled clean and
// ran, silently returning 0 (a wrong value) rather than crashing, because
// the statement-position twin never reaches codegen's DestructuringPattern
// lowering at all when `fields` stays empty (sema's own `recTy == nullptr`
// short-circuits the bind, per the `if (recTy) { ... }` guard around the
// field-collection loop) — a materially different pre-fix failure MODE from
// the expression-position twin, both closed by the same fix.
BEZEL_TEST(type_alias_destructuring_pattern_4050,
           casestmt_record_alias_binds_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasPointStmt4050 := PointStmt4050; "
        "public record PointStmt4050(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Object := new PointStmt4050(3, 4); "
        "    case p of "
        "      AliasPointStmt4050(x, y) => do return x + y; end, "
        "      _ => do return 0; end "
        "    end case; "
        "  end Main; "
        "end C;"), 7);
}

// §19.3 / §27.8: a CLASS alias used as a destructuring head must get the
// SAME "not valid on a class type" rejection a direct class name already
// gets — codegen never even reaches the DestructuringPattern lowering for
// this shape (sema rejects it first), so this row exercises ONLY the sema
// `isClassHead` fix, not the codegen `patternMatches` fix.
BEZEL_TEST(type_alias_destructuring_pattern_4050,
           caseexpr_class_alias_rejected_not_crashed) {
    auto r = runAot(
        "module M; "
        "type AliasDogDestr4050 := DogDestr4050; "
        "public class DogDestr4050 "
        "  destructor end destructor; "
        "end DogDestr4050; "
        "class C "
        "  public static method Main(): int "
        "    using var d: DogDestr4050 := new DogDestr4050(); "
        "    var o: Object := d; "
        "    return (case o of AliasDogDestr4050(_) => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassDestructureMsg));
}

// §19.3 / §27.8: NESTED destructuring via an alias
// (`Outer(AliasOfInner(a, b), c)`) reaches `bindNestedDestructuringNames`'s
// `fieldsOfRecord`, a THIRD, independent sema resolution site fixed the
// same way.
BEZEL_TEST(type_alias_destructuring_pattern_4050,
           nested_destructure_record_alias_binds_correct_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasInner4050 := InnerRec4050; "
        "public record InnerRec4050(V: int); "
        "public record OuterRec4050(Inner: InnerRec4050, W: int); "
        "class C "
        "  public static method Main(): int "
        "    var o := new OuterRec4050(new InnerRec4050(5), 9); "
        "    return (case o of OuterRec4050(AliasInner4050(v), w) => v + w, "
        "                       _ => 0); "
        "  end Main; "
        "end C;"), 14);
}

// Anti-regression: the DIRECT (non-alias) forms must still behave
// identically — pins the fix's new alias arms did not disturb the existing
// TypeSymbol paths in either sema or codegen.
BEZEL_TEST(type_alias_destructuring_pattern_4050,
           direct_record_destructure_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record PointDirect4050(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Object := new PointDirect4050(3, 4); "
        "    return (case p of PointDirect4050(x, y) => x + y, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(type_alias_destructuring_pattern_4050,
           direct_class_destructure_still_rejected) {
    auto r = runAot(
        "module M; "
        "public class DogDirect4050 "
        "  destructor end destructor; "
        "end DogDirect4050; "
        "class C "
        "  public static method Main(): int "
        "    using var d: DogDirect4050 := new DogDirect4050(); "
        "    var o: Object := d; "
        "    return (case o of DogDirect4050(_) => 1, _ => 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassDestructureMsg));
}

// ── KNOWN-OPEN CONTROL for #4047 ─────────────────────────────────────────
//
// Same #4047 declaration-order hazard as #4051's control: a CHAINED alias
// (outer declared textually before inner) leaves
// `sema::resolveAliasToTypeSymbol` returning null for BOTH the sema arms and
// codegen's `patternMatches` arm, so the ORIGINAL crash reproduces.
BEZEL_TEST(type_alias_destructuring_pattern_4050,
           chained_alias_REVERSE_ORDER_destructure_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasPointRevOuter4050 := AliasPointRevInner4050; "
        "type AliasPointRevInner4050 := PointRevOuter4050; "
        "public record PointRevOuter4050(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Object := new PointRevOuter4050(3, 4); "
        "    return (case p of AliasPointRevOuter4050(x, y) => x + y, _ => 0); "
        "  end Main; "
        "end C;");
    // KNOWN-OPEN (#4047): correct behavior is 7 (matching the
    // inner-before-outer order above); measured actual is a compiler CRASH
    // (`!r.ran`, no clean diagnostic) — the exact pre-#4050-fix symptom,
    // inherited from the upstream ordering gap.
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "does not have terminator") ||
                       diagnosticMessageContains(r, "IR verification failed"));
}
