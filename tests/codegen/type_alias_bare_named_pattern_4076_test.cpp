// Spec-derived tests for fix #4076 — §19.3 / §19.5 / §27.8 (v0.294): a bare
// (non-destructuring) `type`-alias type-test pattern (`case x of AliasOfFoo
// => ...`, no parentheses) was correctly ACCEPTED by sema
// (`ExprTC::checkTopLevelNamedPattern`, ExprTypecheck.cpp — it already had a
// `TypeAliasSymbol` "type-alias type test" arm) but SPURIOUSLY REJECTED by
// codegen, which has two independent name-resolution sites for exactly this
// pattern shape and neither recognized a `TypeAliasSymbol`:
//
//   * `Emitter::descriptorForPatternName` (LlvmBackend_Setup.cpp) — resolves
//     a bare pattern name to a class/interface descriptor for the runtime
//     `emitIsA` type test. Reached first, for a CLASS/INTERFACE alias.
//   * `Emitter::patternMatches`'s bare-`NamedPattern` record/enum loop
//     (LlvmBackend_Exprs2.cpp, fix #1206) — decides a RECORD/ENUM alias
//     statically (records/enums have no vtable, so the runtime is-a path
//     above is wrong for them).
//
// Both fell through to the generic #1847 backstop diagnostic ("not
// supported by the backend"), a FALSE REJECT of a sema-accepted program —
// not a crash, not a wrong value: the compile itself spuriously failed.
//
// This is the sixth member of the #4038/#4048/#4050/#4051/#4052 type-alias
// defect family and, like #4050's DestructuringPattern gap, PROVES the same
// point a third time: codegen's `patternMatches` does NOT consume sema's
// resolution here — it independently RE-RESOLVES the written name against
// `ctx_.symbols()`, so the sema-side fix alone (already present before this
// commit) left this false reject reproducing. Both sites are fixed here,
// resolving a `TypeAliasSymbol` through the shared free function
// `sema::resolveAliasToTypeSymbol` (TypeResolution.hpp/.cpp) — the SAME
// seam #4050/#4051 introduced — rather than each hand-rolling a second copy.
//
// Distinguished from #4062 (a DIFFERENT, complementary finding): #4062 is
// about the `ConstantPattern` alt-list lowering's `if (!cv) return nullptr;`
// having no diagnostic and no never-match guard for an unhandled alt kind —
// a silent-degrade hardening concern. #4076's bug is a false reject of
// legitimate, sema-accepted code; a guard alone would be the wrong fix here
// (it would keep rejecting cleanly instead of making the program work) —
// only resolving the alias, as this fix does, makes `case p of AliasOfFoo`
// actually compile and compute the correct value.

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
// The exact #1847 backstop wording (LlvmBackend_Exprs2.cpp) both sites fell
// through to pre-fix.
constexpr const char* kBackstopMsg = "not supported by the backend";
}  // namespace

// #4076 issue repro, verbatim (§19.3/§19.5/§27.8): a bare RECORD-alias
// type-test pattern must MATCH and compute the correct value — 7, exactly
// like the direct (unaliased) form — not fall through to the #1847
// backstop. Pre-fix: compile error at the `case` arm.
BEZEL_TEST(type_alias_bare_named_pattern_4076, record_alias_bare_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasPt4076 := Pt4076; "
        "public record Pt4076(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt4076(1, 2); "
        "    return (case p of AliasPt4076 => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

// §19.5 (#1206's sameType=false path, through an alias): a bare RECORD-alias
// pattern naming a DIFFERENT record than the scrutinee's static type must
// NEVER match — the wildcard arm fires, value 0. Guards against a fix that
// merely makes the alias arm always-true rather than resolving to the real
// aliased TypeSymbol and comparing identity.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           record_alias_bare_nonmatch_falls_through) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasQt4076 := Qt4076; "
        "public record Pt4076B(x: int, y: int); "
        "public record Qt4076(a: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt4076B(1, 2); "
        "    return (case p of AliasQt4076 => 5, _ => 0); "
        "  end Main; "
        "end C;"), 0);
}

// Anti-regression: the DIRECT (non-alias) record form must still behave
// identically — pins that the new alias arm in the record/enum loop
// (LlvmBackend_Exprs2.cpp) did not disturb the pre-existing `TypeSymbol`
// path fix #1206 established.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           record_direct_bare_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record PtDirect4076(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new PtDirect4076(1, 2); "
        "    return (case p of PtDirect4076 => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}

// #4076 issue repro (§10/§19.3/§27.8): a bare ENUM-alias type-test pattern
// must match and yield the correct value — 42, exactly like the direct
// enum form.
BEZEL_TEST(type_alias_bare_named_pattern_4076, enum_alias_bare_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasE4076 := E4076; "
        "public enum E4076 A, B, C; end E4076; "
        "class C "
        "  public static method Main(): int "
        "    var e := E4076.B; "
        "    return (case e of AliasE4076 => 42, _ => 0); "
        "  end Main; "
        "end C;"), 42);
}

// Anti-regression: the DIRECT (non-alias) enum form still behaves
// identically.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           enum_direct_bare_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public enum EDirect4076 A, B, C; end EDirect4076; "
        "class C "
        "  public static method Main(): int "
        "    var e := EDirect4076.B; "
        "    return (case e of EDirect4076 => 42, _ => 0); "
        "  end Main; "
        "end C;"), 42);
}

// #4076 issue repro (§19.3/§27.8): a bare CLASS-alias type-test pattern over
// a boxed (`Object`-typed) scrutinee must resolve through
// `descriptorForPatternName`'s new `TypeAliasSymbol` arm and run the SAME
// runtime `emitIsA` test the direct class name already gets — value 1.
BEZEL_TEST(type_alias_bare_named_pattern_4076, class_alias_bare_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasDog4076 := Dog4076; "
        "public class Dog4076 "
        "  public method Bark(): int return 1; end Bark; "
        "end Dog4076; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new Dog4076(); "
        "    return (case o of AliasDog4076 => 1, _ => 0); "
        "  end Main; "
        "end C;"), 1);
}

// §19.3: a bare CLASS-alias pattern naming an UNRELATED class than the
// boxed scrutinee's runtime type must never match — value 0. Guards against
// a fix that resolves the descriptor but always reports a match regardless
// of the runtime `emitIsA` outcome.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           class_alias_bare_nonmatch_falls_through) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasCat4076 := Cat4076; "
        "public class DogB4076 "
        "  public method Bark(): int return 1; end Bark; "
        "end DogB4076; "
        "public class Cat4076 "
        "  public method Meow(): int return 1; end Meow; "
        "end Cat4076; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new DogB4076(); "
        "    return (case o of AliasCat4076 => 1, _ => 0); "
        "  end Main; "
        "end C;"), 0);
}

// Anti-regression: the DIRECT (non-alias) class form still behaves
// identically — pins that the new alias arm in `descriptorForPatternName`
// did not disturb the pre-existing `TypeSymbol` class/interface path.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           class_direct_bare_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class DogDirect4076 "
        "  public method Bark(): int return 1; end Bark; "
        "end DogDirect4076; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := new DogDirect4076(); "
        "    return (case o of DogDirect4076 => 1, _ => 0); "
        "  end Main; "
        "end C;"), 1);
}

// ── KNOWN-OPEN CONTROL for #4047 ─────────────────────────────────────────
//
// Same #4047 declaration-order hazard as #4050's/#4051's own controls: a
// CHAINED alias (outer declared textually BEFORE inner) leaves
// `sema::resolveAliasToTypeSymbol` returning null for this bare-pattern
// codegen arm too (`ctx_.resolvedType(*aliasDecl->aliasedType)` was never
// populated for the outer alias — `Resolver::resolveTopDecl`'s single
// forward pass processes the outer alias before the inner one it points to
// has resolved its OWN `aliasedType`). Measured: a DIRECT alias-of-record
// (not chained) is unaffected by declaration order — `resolveAliasToTypeSymbol`
// only depends on the alias's own `aliasedType`, which is populated
// regardless of whether the RECORD's own textual declaration comes before
// or after the alias (its TypeSymbol is registered in an earlier
// discovery pass) — this row isolates the CHAINED case specifically.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           chained_alias_outer_first_bare_pattern_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasChainOuter4076 := AliasChainInner4076; "
        "type AliasChainInner4076 := PtChain4076; "
        "public record PtChain4076(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new PtChain4076(1, 2); "
        "    return (case p of AliasChainOuter4076 => 7, _ => 0); "
        "  end Main; "
        "end C;");
    // KNOWN-OPEN (#4047): correct behavior is 7 (matching the
    // inner-before-outer order pinned below); measured actual is the SAME
    // clean #1847 "not supported by the backend" rejection #4076 itself
    // fixes for the non-chained case, inherited from the upstream
    // #4047 ordering gap in `resolveAliasToTypeSymbol`'s single-pass
    // dependency on `ctx_.resolvedType`.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBackstopMsg));
}

// Companion to the KNOWN-OPEN row above: the WORKING declaration order
// (inner alias declared before the outer one that points to it) resolves
// correctly through the chain and computes 7 — confirms the gap above is
// specifically about declaration ORDER, not chained aliases in general.
BEZEL_TEST(type_alias_bare_named_pattern_4076,
           chained_alias_inner_first_bare_pattern_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasChainInnerB4076 := PtChainB4076; "
        "type AliasChainOuterB4076 := AliasChainInnerB4076; "
        "public record PtChainB4076(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new PtChainB4076(1, 2); "
        "    return (case p of AliasChainOuterB4076 => 7, _ => 0); "
        "  end Main; "
        "end C;"), 7);
}
