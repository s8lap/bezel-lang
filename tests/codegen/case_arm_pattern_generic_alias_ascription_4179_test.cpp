// Spec-derived tests for issue #4179 — §19.3 ("Type patterns with
// binding") / §27.8 (generic type aliases, v0.298): a `case`-arm pattern's
// type ascription (`var b: Alias<T>`) resolved through a GENERIC type
// alias must see through to the alias's real instantiated underlying type,
// exactly as §27.8 requires: "Each instantiation of the alias is
// interchangeable with the corresponding instantiation of the aliased
// type" and "Type aliases — generic or not — see through to the aliased
// type for identity, equality, method dispatch, and overload resolution."
//
// Pre-fix (base commit 3999278), the alias-typed arm's binding kept the
// alias's own UNBOUND generic-parameter marker instead of the reference
// site's concrete argument — the SAME root cause fix #4188 (§27.8, v0.298,
// commit f9892ac) measured and fixed in `Resolver::resolveNamed`'s
// alias-transparency copy (`src/sema/TypeResolution_Names.cpp`): that
// branch copied the alias's CACHED underlying `TypeRef` verbatim, and that
// cached tree was resolved once at the alias's own declaration against a
// scope binding the alias's generic parameters to nullptr markers. So the
// case-arm binding's static type never became the real `IBoxG6<int>`, and
// the whole `case` expression's arms had "no common type" with the `_`
// arm's `int` — a hard §20 compile reject — even though the identical
// pattern ascribed with the alias's directly-spelled-out underlying type
// compiles and dispatches correctly (the `direct_type_...` control below).
//
// Verified: #4188's fix (commit f9892ac) already closes this gap — it is
// NOT a separate defect needing its own fix. `Resolver::substituteAliasGenericArgs`
// is invoked from the one `resolveNamed` alias-transparency call site that
// every reference position shares (parameter, return, field, local,
// case-arm pattern type alike), so the case-arm position was never a
// distinct code path — the issue's "measured, not root-caused" note was
// correct to suspect #4172/#4188's root cause, and it *is* that cause,
// confirmed by building bezelc at both 3999278 (reject) and f9892ac
// (accept, correct value) and running the repro below unchanged.
//
// This file therefore ships no compiler change of its own — it is the
// spec-cited regression test for #4179 riding on top of #4188's fix,
// proven FAIL-WITHOUT (base 3999278) / PASS-WITH (f9892ac) mechanically
// against two independently rebuilt compilers; see this commit's message
// for the measured counts. Only ONE of the four rows below actually
// reproduces the bug (the expression-position primary row) — the
// statement-position row was hypothesized to reproduce it too and
// measurably does NOT (see that row's own comment for why); the other two
// rows are deliberate accept-side controls that pass identically on both
// compilers by design.

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// §19.3 / §27.8 / #4179 issue repro verbatim (identifiers suffixed
// `_4179` for uniqueness): a case-arm binding's declared type, ascribed
// through a GENERIC alias instantiation over an interface, must dispatch
// through the alias's real underlying type. Pre-fix (3999278): "the arms
// of a `case` expression have no common type: an arm's type is not
// implicitly convertible to that of the other arms (§20)" compile reject
// at the arm's source location. Post-fix (f9892ac): compiles and runs,
// returning 6 (`ImplBoxG6_4179.Get()`'s value) — identical to the
// direct-type control immediately below.
BEZEL_TEST(case_arm_pattern_generic_alias_ascription_4179,
           alias_typed_case_arm_binding_dispatches_through_alias) {
    const std::string prog =
        "module M; "
        "public interface IBoxG6_4179<T> "
        "  method Get(): T; "
        "end IBoxG6_4179; "
        "public class ImplBoxG6_4179 : IBoxG6_4179<int> "
        "  public method Get(): int => 6; "
        "end ImplBoxG6_4179; "
        "type IBoxAliasG6_4179<T> := IBoxG6_4179<T>; "
        "public static class ProgramG6_4179 "
        "  public static method Main(): int "
        "    var t: IBoxG6_4179<int> := new ImplBoxG6_4179(); "
        "    return (case t of "
        "      var b: IBoxAliasG6_4179<int> => b.Get(), "
        "      _ => -1, "
        "    ); "
        "  end Main; "
        "end;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// Control: the IDENTICAL program with the case-arm binding's type spelled
// out directly (`IBoxG6_4179b<int>`, no alias involved) instead of through
// an alias. This always worked, on both sides — it is the row the issue
// itself measured ("Replacing IBoxAliasG6<int> with the concrete
// IBoxG6<int> ... compiles and runs, returning 6"). Confirms the failure
// is specific to routing the pattern's ascribed type through the alias,
// not to interface-typed case-arm dispatch in general.
BEZEL_TEST(case_arm_pattern_generic_alias_ascription_4179,
           direct_type_case_arm_binding_control) {
    const std::string prog =
        "module M; "
        "public interface IBoxG6_4179b<T> "
        "  method Get(): T; "
        "end IBoxG6_4179b; "
        "public class ImplBoxG6_4179b : IBoxG6_4179b<int> "
        "  public method Get(): int => 6; "
        "end ImplBoxG6_4179b; "
        "public static class ProgramG6_4179b "
        "  public static method Main(): int "
        "    var t: IBoxG6_4179b<int> := new ImplBoxG6_4179b(); "
        "    return (case t of "
        "      var b: IBoxG6_4179b<int> => b.Get(), "
        "      _ => -1, "
        "    ); "
        "  end Main; "
        "end;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// §19.1: the STATEMENT-position `case` form (`CaseStmt`, grammar A.6) is a
// separate grammar production from the expression-position atom above,
// with arms that are single statements rather than expression bodies.
// MEASURED FINDING (not the row's original hypothesis): this program
// compiles and runs correctly on BOTH sides of the fix, including at base
// 3999278 — it does NOT reproduce #4179. Statement-position arms are typed
// independently (§21.9, no cross-arm common-type unification), so
// `result := b.Get()` only needs the assignment's own implicit-conversion
// check, which is lenient enough pre-fix to accept the alias-typed `b`
// even with its `genericArgs` still `[T-unbound]`. #4179's actual trigger
// is specifically the EXPRESSION form's stricter §20 arms-have-no-common-
// type unification (the row above) — this row is therefore a genuine
// control pinning that scope, not a second repro: it must keep passing
// identically pre- and post-fix.
BEZEL_TEST(case_arm_pattern_generic_alias_ascription_4179,
           alias_typed_case_arm_binding_statement_position) {
    const std::string prog =
        "module M; "
        "public interface IBoxG6_4179c<T> "
        "  method Get(): T; "
        "end IBoxG6_4179c; "
        "public class ImplBoxG6_4179c : IBoxG6_4179c<int> "
        "  public method Get(): int => 6; "
        "end ImplBoxG6_4179c; "
        "type IBoxAliasG6_4179c<T> := IBoxG6_4179c<T>; "
        "public static class ProgramG6_4179c "
        "  public static method Main(): int "
        "    var t: IBoxG6_4179c<int> := new ImplBoxG6_4179c(); "
        "    var result := -1; "
        "    case t of "
        "      var b: IBoxAliasG6_4179c<int> => result := b.Get(), "
        "      _ => result := -2 "
        "    end case; "
        "    return result; "
        "  end Main; "
        "end;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// Control: a NON-generic alias at case-arm binding position must keep
// working on both sides of the fix. #4188's fix scopes its substitution
// map to a GENERIC alias's declared parameters
// (`aliasDecl->genericParams`); a non-generic alias's substitution map is
// empty (fast path, no allocation, `substituteAliasGenericArgs` returns
// its input unchanged) — this row pins that the fix did not disturb the
// already-working non-generic case-arm alias pattern.
BEZEL_TEST(case_arm_pattern_generic_alias_ascription_4179,
           non_generic_alias_case_arm_binding_unaffected) {
    const std::string prog =
        "module M; "
        "public interface IBoxPlain_4179d "
        "  method Get(): int; "
        "end IBoxPlain_4179d; "
        "public class ImplBoxPlain_4179d : IBoxPlain_4179d "
        "  public method Get(): int => 6; "
        "end ImplBoxPlain_4179d; "
        "type IBoxAliasPlain_4179d := IBoxPlain_4179d; "
        "public static class ProgramPlain_4179d "
        "  public static method Main(): int "
        "    var t: IBoxPlain_4179d := new ImplBoxPlain_4179d(); "
        "    return (case t of "
        "      var b: IBoxAliasPlain_4179d => b.Get(), "
        "      _ => -1, "
        "    ); "
        "  end Main; "
        "end;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 6);
}
