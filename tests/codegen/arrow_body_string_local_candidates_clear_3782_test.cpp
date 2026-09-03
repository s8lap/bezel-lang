// Codegen / invariant pin — issue #3782 (spec §28.9 "Runtime-managed types",
// v0.286): `emitFunctionBody`'s per-body setup cleared `stringReleaseEligible_`
// and re-pinned `stringReleaseEligibleFn_ = fn` unconditionally, but left
// `stringLocalCandidates_` (fix #2817's body-membership set) holding whatever
// the PREVIOUS body's scan put there — the exact two-halves clear()+populate
// asymmetry the #3757/#3762 fix closed ~30 lines below for the closure twin
// `closureLocalCandidates_`, unfixed for the string one.
//
// `stringLocalCandidates_` is repopulated ONLY by `collectStringReleaseEligible`,
// which `emitFunctionBody` calls under a `ci.bodyKind == Block && ci.stmts`
// gate. An EXPRESSION-bodied (arrow) method never runs it, yet the fn token IS
// pinned for that body — so `returnedStringLocalOwnsPlusOne`'s fn gate is
// satisfied while its membership set describes a different body. The arrow-body
// string mint (`#694` / fix #2262, the `ci.exprBody` site) consults that
// predicate directly, and a stale hit there SKIPS the +1 the caller's scope-end
// release is about to discharge — the under-retain / premature-free direction.
//
// NOT EXPLOITABLE TODAY, by accident rather than by invariant: the predicate
// requires the returned identifier to resolve to a `sema::LocalSymbol`, an
// arrow body declares no locals of its own, and `ParameterSymbol` is not a
// `LocalSymbol` — so the only names an arrow body can return (parameters and
// members) cannot reach the candidate set. Nothing pins that. Objects are
// byte-identical with and without the fix (measured, `cmp` over every program
// in this file plus the #3773 grid), so these rows CANNOT fail-without: they
// pin the behaviour the invariant protects — the arrow-body mint's exact
// refcount accounting after a block-bodied neighbour populated the set — so
// that the refactor which makes the conflation reachable breaks a test instead
// of a program.
//
// Every row is `runAotChecked` (generated-code ASan + LeakSanitizer), 200
// activations. An arrow body that skipped its mint frees the field's / the
// caller's string one release early (ASan use-after-free or double-free); one
// that minted twice leaks. Both sides of the invariant are visible.
// per §28.9 (issue #3782)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

const char* kHdr = "module M; import System; ";

const char* kTail =
    "    Console.WriteLine(\"total=\" + total.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1: a BLOCK-bodied method declared FIRST, whose body binds two
// producer-initialized `string` locals — so its `collectStringReleaseEligible`
// scan leaves `stringLocalCandidates_` non-empty — followed by an ARROW-bodied
// `string`-returning method whose own setup never re-runs that scan. The arrow
// body returns its own PARAMETER, a borrow that must be minted +1 for the
// caller's `var` bind to release. Prime(): "ab"+"c" and "de"+"f" -> 3+3 = 6.
// Echo(p) returns p; the caller binds and measures it. 200 * (6 + 4) = 2000.
// per §28.9 (issue #3782)
BEZEL_TEST(arrow_body_string_local_candidates_clear_3782,
           arrow_body_param_return_mints_after_block_body_neighbour_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Prime(t: string): int "
        "    var a := \"ab\" + t; "
        "    var b := \"de\" + t; "
        "    return a.Length + b.Length; "
        "  end Prime; "
        "  public static method Echo(p: string): string => p; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      total := total + C.Prime(\"c\"); "
        "      var s := C.Echo(\"wxyz\"); "
        "      total := total + s.Length; "
        "    end; " +
        kTail),
        "total=2000\n");
}

// ── Row 2: the same ordering, but the arrow body returns a `string` FIELD of
// `this` — the shape #694's own comment names ("a `=> fieldRef` or `=> param`
// body returns a raw borrow"). A skipped mint frees the field's string under
// the object; a doubled one leaks it. Field is "abc"+t (4 chars); 200 * 4.
// per §28.9 (issue #3782)
BEZEL_TEST(arrow_body_string_local_candidates_clear_3782,
           arrow_body_field_return_mints_after_block_body_neighbour_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  private sf: string; "
        "  public constructor(t: string) "
        "    sf := \"abc\" + t; "
        "  end constructor; "
        "  public method Prime(): int "
        "    var a := \"ab\" + sf; "
        "    return a.Length; "
        "  end Prime; "
        "  public method Get(): string => this.sf; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      using var c := new C(\"z\"); "
        "      var unused := c.Prime(); "
        "      var s := c.Get(); "
        "      total := total + s.Length; "
        "    end; " +
        kTail),
        "total=800\n");
}

// ── Row 3: a `string?` arrow body after the same block-bodied neighbour. The
// nullable half of the arrow-body mint is fix #2262's, and it consults the
// SAME predicate, so it is the second consumer this clear protects. Returns a
// non-null `string?`; the caller's `!` reads it. 200 * 4.
// per §28.9, §7.8 (issue #3782)
BEZEL_TEST(arrow_body_string_local_candidates_clear_3782,
           nullable_arrow_body_return_mints_after_block_body_neighbour_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kHdr) +
        "class C "
        "  public static method Prime(t: string): int "
        "    var a := \"ab\" + t; "
        "    return a.Length; "
        "  end Prime; "
        "  public static method Maybe(p: string): string? => p; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      var unused := C.Prime(\"c\"); "
        "      var s := C.Maybe(\"wxyz\"); "
        "      total := total + s!.Length; "
        "    end; " +
        kTail),
        "total=800\n");
}
