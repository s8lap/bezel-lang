// Spec-derived tests for fix #4082 — §19.5 / §3.6 rule 4 / §27.8 (v0.294):
// a bare type-test pattern over a generic record / enum scrutinee that
// resolves through a `type` alias MUST discriminate by the alias's full
// generic instantiation, not by the underlying declaration's identity alone.
//
// The same identity-based matching fix #4076 added (`scrutTy->declaration ==
// ts` after `resolveAliasToTypeSymbol`) silently inherited the pre-existing
// #1206 "bare pattern ignores type arguments" reading for the direct-name
// form — and the alias route is the ONE place the spec's v1 grammar lets a
// user actually communicate a specific instantiation through a bare pattern
// (`case b of Box<int>` is a parse error; `case b of type AliasBoxInt := Box<int>;
// AliasBoxInt` is not, and §3.6 rule 4 makes Box<int> and Box<string> distinct
// runtime type identities). Pre-fix: a `Box<int>` scrutinee matched an alias
// naming `Box<string>` and the alias arm fired with the wrong value
// (the issue's measured 4 instead of the expected 0).
//
// The fix adds a generic-argument comparison via the shared
// `sema::typeRefEqual` (TypeResolution.hpp) when the bare-pattern loop in
// `Emitter::patternMatches` resolves its match THROUGH a `TypeAliasSymbol`
// (i.e. `sym` itself was a TypeAliasSymbol — the lookup that produced `ts`
// went through `resolveAliasToTypeSymbol`). The direct (unaliased) bare-name
// form has no spelling for type arguments (#1206) and so keeps its
// matches-any-instantiation reading — the issue's own "Control confirming"
// row, repeated below as `direct_bare_pattern_matches_any_instantiation`,
// verifies the fix does NOT regress that #1206 behavior.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.5 / §3.6 rule 4 / §27.8: #4082 issue repro verbatim. A `Box<int>`
// scrutinee must NOT match an alias naming `Box<string>`. Pre-fix: arm
// fired (exit 4). Post-fix: falls through to `_` (exit 0).
BEZEL_TEST(type_alias_generic_bare_pattern_4082,
           box_int_does_not_match_alias_naming_box_string) {
    const std::string prog =
        "module M; "
        "type AliasBoxStr4082 := Box4082<string>; "
        "public record Box4082<T>(v: T); "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box4082<int>(9); "
        "    return (case b of AliasBoxStr4082 => 4, _ => 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 0);
}

// §19.5 / §3.6 rule 4: the inverse — a `Box<int>` scrutinee DOES match an
// alias naming `Box<int>` (the alias's instantiation agrees with the
// scrutinee's). Pre-fix: arm fired (exit 4) for the WRONG reason
// (declaration-identity alone); post-fix: arm still fires (exit 4) for the
// RIGHT reason (full instantiation equality). The pre- and post-fix value
// coincide here, so this row is the "post-fix matches the spec" half of
// the grid — the negative row above is the one that fails pre-fix and
// pins the bug.
BEZEL_TEST(type_alias_generic_bare_pattern_4082,
           box_int_matches_alias_naming_box_int) {
    const std::string prog =
        "module M; "
        "type AliasBoxInt4082 := Box4082<int>; "
        "public record Box4082<T>(v: T); "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box4082<int>(9); "
        "    return (case b of AliasBoxInt4082 => 4, _ => 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 4);
}

// §3.6 rule 4: multi-arg generics — a `Pair<int, string>` scrutinee must NOT
// match an alias naming `Pair<string, int>`. Tests element-wise generic-arg
// comparison (the single-arg case above proves the loop runs at all; this
// row proves a wrong ordering is detected and not silently accepted).
BEZEL_TEST(type_alias_generic_bare_pattern_4082,
           pair_int_string_does_not_match_alias_naming_pair_string_int) {
    const std::string prog =
        "module M; "
        "type AliasPairSI4082 := Pair4082<string, int>; "
        "public record Pair4082<A, B>(a: A, b: B); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair4082<int, string>(7, \"hi\"); "
        "    return (case p of AliasPairSI4082 => 4, _ => 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 0);
}

// §3.6 rule 4: multi-arg generics — the SAME-args case matches. Mirrors the
// single-arg positive row above for two generic parameters.
BEZEL_TEST(type_alias_generic_bare_pattern_4082,
           pair_string_int_matches_alias_naming_pair_string_int) {
    const std::string prog =
        "module M; "
        "type AliasPairSI4082 := Pair4082<string, int>; "
        "public record Pair4082<A, B>(a: A, b: B); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair4082<string, int>(\"hi\", 7); "
        "    return (case p of AliasPairSI4082 => 4, _ => 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 4);
}

// §27.8 / §19.5 control row from the issue: the DIRECT (unaliased) bare
// pattern `Box` (no type args — the only spelling the grammar admits) must
// keep #1206's matches-any-instantiation behavior. This is the load-bearing
// anti-regression for the fix's narrowing — the fix MUST NOT pull the
// declaration-identity-only shortcut out from under the direct-name arm
// while it adds the generic-arg check to the alias arm. Pre-fix: matches
// (exit 4) — kept by the fix (still exit 4).
BEZEL_TEST(type_alias_generic_bare_pattern_4082,
           direct_bare_pattern_matches_any_instantiation) {
    const std::string prog =
        "module M; "
        "public record Box4082<T>(v: T); "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box4082<int>(9); "
        "    return (case b of Box4082 => 4, _ => 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 4);
}

// §27.8 anti-regression: a non-generic alias (`type AliasPt4082 := Pt4082`)
// must keep working — the declaration-identity-only comparison for a
// non-generic alias is still exact, since the alias's underlying TypeRef
// carries no genericArgs to compare. This row proves the fix's
// generic-arg guard is structurally correct (an empty genericArgs vector
// passes through the size check trivially and the loop runs zero
// iterations).
BEZEL_TEST(type_alias_generic_bare_pattern_4082,
           non_generic_alias_of_record_still_matches) {
    const std::string prog =
        "module M; "
        "type AliasPt4082 := Pt4082; "
        "public record Pt4082(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt4082(3, 7); "
        "    return (case p of AliasPt4082 => 4, _ => 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 4);
}
