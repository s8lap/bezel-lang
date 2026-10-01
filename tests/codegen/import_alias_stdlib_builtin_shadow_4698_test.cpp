// Codegen — #4698 review probes (spec §2.2 / §2.7, v0.313): the alias-qualified
// spelling of a compiler-built-in stdlib type must reach the BUILT-IN even
// when a same-named user type is in scope.
//
// SPEC §2.2 (v0.313): "A declaration in the *current module* shadows any
// same-named import for bare-name purposes (the import stays reachable by
// qualifier)" and "`Tx.Ledger` names that module's public `Ledger`". So under
// `import System.Diagnostics as D;`, `D.Stopwatch` is stdlib §8.1's Stopwatch
// even when the module declares its own `class Stopwatch`. A fix that rewrites
// `D.Stopwatch` to the BARE `Stopwatch` re-binds it to the user's class.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// The user's `Stopwatch` has NO `IsRunning`; only the built-in does.
BEZEL_TEST(import_alias_stdlib_builtin_shadow_4698, alias_static_call_reaches_builtin_not_user_class) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Stopwatch "
        "  public constructor() end constructor; "
        "  public static method StartNew(): owned Stopwatch return new Stopwatch(); end StartNew; "
        "end Stopwatch; "
        "class C public static method Main() "
        "  using var s := D.Stopwatch.StartNew(); "
        "  Console.WriteLine(s.IsRunning); "
        "end Main; end C;"), "True");
}

BEZEL_TEST(import_alias_stdlib_builtin_shadow_4698, alias_new_and_annotation_reach_builtin_not_user_class) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class Stopwatch public constructor() end constructor; end Stopwatch; "
        "class C public static method Main() "
        "  using var s: D.Stopwatch := new D.Stopwatch(); "
        "  Console.WriteLine(s.IsRunning); "
        "end Main; end C;"), "False");
}

// A local named like the built-in must not capture `D.Stopwatch` either (§2.7
// rule 1 applies to the chain head `D`, not to the member name).
BEZEL_TEST(import_alias_stdlib_builtin_shadow_4698, alias_static_call_ignores_local_named_stopwatch) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class C public static method Main() "
        "  var Stopwatch := 7; "
        "  using var s := D.Stopwatch.StartNew(); "
        "  Console.WriteLine(s.IsRunning); "
        "  Console.WriteLine(Stopwatch); "
        "end Main; end C;"), "True\n7");
}

// A type parameter named like the built-in (generic method `Go<Stopwatch>`)
// must not capture `D.Stopwatch` in type position (`new D.Stopwatch()`,
// annotation) or in expression position (`D.Stopwatch.StartNew()`).
BEZEL_TEST(import_alias_stdlib_builtin_shadow_4698, alias_ignores_type_parameter_named_stopwatch) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class C "
        "  public static method Go<Stopwatch>(x: Stopwatch) "
        "    using var s := D.Stopwatch.StartNew(); "
        "    Console.WriteLine(s.IsRunning); "
        "    using var t: D.Stopwatch := new D.Stopwatch(); "
        "    Console.WriteLine(t.IsRunning); "
        "  end Go; "
        "  public static method Main() Go<int>(1); end Main; "
        "end C;"), "True\nFalse");
}
