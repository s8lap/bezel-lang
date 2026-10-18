// Cross-module — fix #3044 (§5.4 / §10.4, v0.242): the checked enum cast and
// the compiler-provided `FromUnderlyingOrNull` must work identically for an
// IMPORTED enum.
//
// This is the arm a same-module test never reaches. An imported enum has no
// `ast::TypeDecl` on the importer's side, so its declared-value set cannot be
// re-derived from source: it comes from the reconstructed
// `EnumConstantSymbol::value` the dependency metadata carries (issue #328). The
// checked cast, `FromUnderlyingOrNull`, and §19.6/§21.7's exhaustiveness all
// read that same set — if it were empty or zeroed on this side, the cast would
// either reject every value or accept every value, and neither would be visible
// from a single-module test.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

// ── PORT NOTE (#6386, round 2) ───────────────────────────────────────────────
//
// The two hand-rolled runners — `buildAndRun` (compile lib → compile+link app →
// `std::system` + `WEXITSTATUS`) and `appCompiles` (compile-only verdict) — are
// retired onto `test::runAotModules`. Both also wrote into
// `std::filesystem::temp_directory_path()` (== `/tmp`, which CLAUDE.md forbids);
// `runAotModules` uses a per-call `mkdtemp` under the shared `scratchBase()`.
//
// ⚠ TWO ROWS GET STRONGER, AND ONE OF THEM CHANGES SHAPE.
//
//   * The three RUN rows previously collapsed a compile failure, a link failure,
//     a signal and a real run into one `int` (-1 for all of them), so
//     `BEZEL_EXPECT_EQ(code, 42)` could not say which happened. Each now
//     asserts `r.ran` FIRST and unconditionally, then the exit code — and
//     `r.diagnostics` carries the reason (`compiler CRASHED (signal N)`,
//     `program terminated by signal N`, `did not terminate within`) when it is
//     not a value mismatch.
//
//   * `imported_constant_declared_accepted_spec_5_4` asserted only "the app
//     COMPILES". `r.ran` is strictly stronger: the program must additionally
//     LINK and RUN to completion. Its `Main` returns `int(P(20))` = 20, so the
//     port also asserts that exit code — a new value oracle the old row did not
//     have. (Disclosed as an added assertion, not a rewrite of the old one.)
//
//   * `imported_constant_undeclared_rejected_spec_5_4` asserted only
//     `appCompiles(...) == false`. That is the vacuous-oracle shape: ANY error —
//     including one unrelated to §5.4 — turned it GREEN. It now asserts `!r.ran`
//     plus TWO diagnostic needles read from src/sema/ExprTypecheck_Calls.cpp's
//     `checkEnumCastConstantOperand` ("is not the underlying value of any
//     declared constant of" and "§5.4"), so the row fails unless the cast site
//     itself is rejected for the §5.4 reason.
//
//   * ⚠ THE PRE-EMPTING-CHECK HAZARD IS REAL HERE AND THE NEEDLE RULES IT OUT.
//     §5.4's constant arm only fires when `enumDeclaresValue` can DECIDE the
//     declared set; when it cannot, the code falls back to an underlying-type
//     RANGE check, which rejects a DIFFERENTLY-worded diagnostic for the same
//     program. Asserting the §5.4 wording is what makes the row fail if that
//     fallback ever starts firing — i.e. if the importer's declared-value set
//     stopped being decidable.
//
// No exit code here exceeds 255, so no row is exposed to 8-bit truncation.
// TEST NAMES ARE UNCHANGED (#6386 rule 5).
//
// Spec: §5.4, §10.4, §2.5, §2.6, v0.242, v0.338.

namespace bezel::module {
namespace {

// Sparse on purpose: 10, 20, 30 leaves gaps an in-range value can fall into.
const char* kLib =
    "module PLib; public enum P Low := 10, Mid := 20, High := 30; end P;";

// The two-module program: the library above plus one importer. `runAotModules`
// matches a module to its `import` BY NAME, so `PLib` here must be the module
// name the source declares — a hardcoded name that did not match would make
// every row in this file report the same unrelated "module not found" reject.
inline std::vector<test::ModuleSource> mods3044(const char* appSrc) {
    return {{"PLib", kLib}, {"PApp", appSrc}};
}

}  // namespace

// §5.4 runtime arm, imported enum: an undeclared value raises OverflowException.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_undeclared_raises_spec_5_4) {
    auto r = test::runAotModules(mods3044(
        "module PApp; import PLib; class C "
        "  public static method Main(): int "
        "    try var n: int := 25; var p: P := P(n); return int(p); "
        "    catch OverflowException => return 42; end; "
        "  end Main; end C;"),
        "PApp");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// OVER-REJECTION GUARD: every DECLARED value of the imported enum still casts —
// this is what fails loudly if the importer's declared-value set were empty or
// zeroed rather than the metadata-carried one.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_declared_values_accepted_spec_5_4) {
    auto r = test::runAotModules(mods3044(
        "module PApp; import PLib; class C "
        "  public static method Main(): int "
        "    var a: int := 10; var b: int := 20; var c: int := 30; "
        "    return int(P(a)) - int(P(b)) + int(P(c)) + 22; "  // 10-20+30+22
        "  end Main; end C;"),
        "PApp");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §5.4 constant arm, imported enum: a constant equal to no declared constant is
// a compile error at the cast site, on the importer's side.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_constant_undeclared_rejected_spec_5_4) {
    auto r = test::runAotModules(mods3044(
        "module PApp; import PLib; class C "
        "  public static method Main(): int var p: P := P(25); "
        "  return int(p); end Main; end C;"),
        "PApp");

    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is not the underlying value of any declared constant of"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.4"));
}

// The same shape with a DECLARED constant must still compile — the pair bounds
// the claim to declaredness rather than to "constants of imported enums".
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_constant_declared_accepted_spec_5_4) {
    auto r = test::runAotModules(mods3044(
        "module PApp; import PLib; class C "
        "  public static method Main(): int var p: P := P(20); "
        "  return int(p); end Main; end C;"),
        "PApp");

    // STRONGER than the old `appCompiles(...) == true`: the program must not
    // only compile and link but RUN, and must observe the cast's value.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// §10.4 / stdlib §9.9: the compiler-provided branch tier over an imported enum.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_from_underlying_or_null_spec_10_4) {
    auto r = test::runAotModules(mods3044(
        "module PApp; import PLib; class C "
        "  public static method Main(): int "
        "    var gap: int := 25; var hit: int := 30; "
        "    var a := P.FromUnderlyingOrNull(gap); "
        "    var b := P.FromUnderlyingOrNull(hit); "
        "    return int(a = null) * 12 + int(b!); "        // 12 + 30
        "  end Main; end C;"),
        "PApp");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

}  // namespace bezel::module
