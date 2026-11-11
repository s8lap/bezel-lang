// Regression test for issue #6933 (§14.1 / §14.2 / §8.1 / §8.5, v0.357).
//
// The CROSS-MODULE half of #6933 — the shape where the erased generic
// template is actually EXECUTED.
//
// Every generic declaration is emitted once unspecialized, with its type
// parameters erased to the `int` (i32) placeholder. That template is not dead
// code: an importing module binds `Gen<int>` to the exported body — `int`
// being the only binding the #1943 / #6779 / #6818 gates let cross the module
// boundary — so the i32 placeholder IS the `int` the template carries there.
//
// `x.ToString()` on a bare `T` receiver inside that template had no
// lowering: both ToString redirects need an active substitution
// (`genericSubstSemaTy_`), and the erased template has none. The call fell
// through to the end of the dispatch chain, reported the FALSE
// "internal: codegen cannot dispatch member 'ToString' on receiver of type
// 'T'" line (§8.1 declares the member on every `T`; the compiler had no
// dispatch for it there) and returned null.
//
// So the imported template computed the WRONG VALUE — measurably, not
// theoretically:
//
//   * `Gen<T>(x: T): int => x.ToString().Length` returned 0 where the value
//     is 1 (the §8.5 decimal rendering of `5` is "5", whose length is 1);
//   * `Str<T>(x: T): string => x.ToString()` returned a NULL string, and the
//     importer's first read of it faulted (a SEGV on the null string header).
//
// The rows below pin the VALUE, not merely "did not crash": the expected
// outputs are the ones only the correct lowering produces.
//
// The sibling §8.1 member on the same shape (`x.GetHashCode()`) was fixed by
// #6907; the same-module half of #6933 (the compile that STOPPED with the
// #1386 internal error) is in
// `tests/codegen/generic_bare_typeparam_tostring_template_6933_test.cpp`.

#include <string>

#include "support/aot_support.hpp"  // runAotModules / runAotModulesChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

// The exported library: the generic methods whose erased template the
// importer binds to, plus the `U.Record` sink.
const char* kLib =
    "module L6933;\n"
    "public static class U\n"
    "  public static method Record(n: int) Console.WriteLine($\"rec {n}\");"
    " end Record;\n"
    "end U;\n"
    "public class G\n"
    "  public static method Len<T>(x: T): int\n"
    "    return x.ToString().Length;\n"
    "  end Len;\n"
    "  public static method Str<T>(x: T): string\n"
    "    return x.ToString();\n"
    "  end Str;\n"
    "  public static method Hash<T>(x: T): int\n"
    "    return x.GetHashCode();\n"
    "  end Hash;\n"
    "end G;\n";

// The importer. The library's own template is what runs here.
const char* kApp =
    "module A6933;\n"
    "import L6933;\n"
    "public class P\n"
    "  public static method Main(): int\n"
    "    U.Record(G.Len(5));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end P;\n";

JitResult run(const std::string& app) {
    return bezel::test::runAotModules({{"L6933", kLib}, {"A6933", app}},
                                       "A6933");
}

std::string out(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// The value the LIVE erased template computes.
// ---------------------------------------------------------------------------

// `G.Len(5)` — the imported binding of `Len<int>` — must be the §8.5 decimal
// rendering's length: `5` renders as "5", so the answer is 1.
//
// BEFORE THE FIX: the erased template degraded its `x.ToString()` to a
// zero/null value, so the answer was 0 — the program ran, and ran WRONG.
BEZEL_TEST(xmod_generic_method_tostring_6933,
           imported_generic_template_length_is_the_int_rendering) {
    JitResult r = run(kApp);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "rec 1\n");
}

// `G.Str(5)` — the imported binding of `Str<int>` — must be the §8.5 decimal
// rendering of the placeholder, "5".
//
// BEFORE THE FIX: the erased template returned a NULL string and the
// importer's read of it faulted (a SEGV on the null string header). This row
// therefore pins a CRASH becoming a correct value — "exit 0" alone would not
// distinguish the two, so the stdout is asserted.
BEZEL_TEST(xmod_generic_method_tostring_6933,
           imported_generic_template_string_return_is_the_int_rendering) {
    const std::string app =
        "module A6933;\n"
        "import L6933;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var s := G.Str(5);\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    JitResult r = run(app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "5\n");
}

// The same call in a LOOP: the digit-lengths of 0..39 sum to 70 (ten
// one-digit and thirty two-digit values). Before the fix every iteration
// answered 0, so the sum was 0.
//
// The loop also amplifies a per-call leak (the #2475 idiom: a one-shot
// allocation can hide under the sanitizer's noise), so this row is run under
// the ASan/LSan oracle.
BEZEL_TEST(xmod_generic_method_tostring_6933,
           imported_generic_template_length_sum_is_the_digit_lengths) {
    const std::string app =
        "module A6933;\n"
        "import L6933;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var count := 0;\n"
        "    var i := 0;\n"
        "    while i < 40 do\n"
        "      count := count + G.Len(i);\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    return count;\n"
        "  end Main;\n"
        "end P;\n";
    JitResult r = bezel::test::runAotModulesChecked(
        {{"L6933", kLib}, {"A6933", app}}, "A6933");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
}

// The string-returning template in a LOOP: 40 renderings, each consumed by
// a `Length` read. Under the ASan/LSan oracle this pins that the fresh
// rc=1 string the template now mints is discharged exactly once — a leak
// would flip `r.ran` to false, and an over-release would fault.
//
// The same shape on a CONCRETE `int` receiver is the control below: it was
// already leak-clean before this fix, so this row asserts that the erased
// template's new dispatch is leak-clean on the SAME terms.
BEZEL_TEST(xmod_generic_method_tostring_6933,
           imported_generic_template_string_return_is_leak_free) {
    const std::string app =
        "module A6933;\n"
        "import L6933;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var count := 0;\n"
        "    var i := 0;\n"
        "    while i < 40 do\n"
        "      var s := G.Str(i);\n"
        "      if s.Length > 0 then count := count + 1; end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    return count;\n"
        "  end Main;\n"
        "end P;\n";
    JitResult r = bezel::test::runAotModulesChecked(
        {{"L6933", kLib}, {"A6933", app}}, "A6933");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ---------------------------------------------------------------------------
// CONTROLS — green on BOTH arms.
// ---------------------------------------------------------------------------

// The sibling §8.1 member on the same shape, fixed by #6907: the imported
// template hashes the `int` placeholder as the `int` it carries (§8.3: an
// `int`'s hash is its value). This row must be unchanged by #6933 — it is the
// admitting twin of the rows above (the same shape, the member that already
// had a dispatch).
BEZEL_TEST(xmod_generic_method_tostring_6933,
           control_imported_generic_template_hashcode_is_the_int_value) {
    const std::string app =
        "module A6933;\n"
        "import L6933;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(G.Hash(5));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    JitResult r = run(app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "5\n");
}

// A CONCRETE `int` parameter of a NON-generic exported method: the same
// rendering, reached without any type parameter at all. It was already
// correct before this fix, so it pins that the new arm does not disturb the
// ordinary path.
BEZEL_TEST(xmod_generic_method_tostring_6933,
           control_imported_concrete_int_receiver_is_the_int_rendering) {
    const std::string lib =
        "module L6933;\n"
        "public class G\n"
        "  public static method Plain(x: int): string\n"
        "    return x.ToString();\n"
        "  end Plain;\n"
        "end G;\n";
    const std::string app =
        "module A6933;\n"
        "import L6933;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var s := G.Plain(5);\n"
        "    Console.WriteLine(s);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    JitResult r = bezel::test::runAotModules({{"L6933", lib}, {"A6933", app}},
                                              "A6933");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "5\n");
}
