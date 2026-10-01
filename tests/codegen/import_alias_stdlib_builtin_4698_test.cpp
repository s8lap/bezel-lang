// Codegen — #4698 (spec §2.2, v0.313): an import alias reaches a stdlib
// module's public names by qualification.
//
// SPEC §2.2 (v0.313): "An import may bind a file-scoped alias: `import
// BankingApp.Transactions as Tx;` — thereafter `Tx.Ledger` names that module's
// public `Ledger`. The alias is an additional *qualifier*, not a rename: the
// module's publics are still visible by bare name ..."
//
// So under `import System.Diagnostics as D;`, `D.Stopwatch` names the same
// entity as the bare `Stopwatch` (stdlib §8.1), and `import System.Threading
// as Th;` makes `Th.CancellationTokenSource` (stdlib §6.1) the bare form's twin.
//
// Pre-fix, the compiler-built-in types (no exported TypeSymbol in their module)
// were mishandled when alias-qualified:
//   * expression position (`D.Stopwatch.StartNew()`): sema accepted it, codegen
//     could not lower the head, and the local was silently ZERO-INITIALIZED
//     ("internal: lowerLocalDecl's ... degraded to a zero-initialized value",
//     #2434) with exit 0 — silent wrong code;
//   * type position (`Th.CancellationTokenSource`): rejected "module
//     `System.Threading` has no public type ..." while the bare form compiled.
//
// The oracle is stdout VALUES that only a real object can produce (runAot
// reports diagnostics only on reject, so a warning-absence check would be
// vacuous). A zero-initialized Stopwatch / source reads IsRunning=False /
// Token.IsCancellationRequested=False and never prints `True`.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// Control: the bare form under a plain import (what the alias must equal).
BEZEL_TEST(import_alias_stdlib_builtin_4698, control_bare_stopwatch_startnew) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics; "
        "class C public static method Main() "
        "  using var s := Stopwatch.StartNew(); "
        "  Console.WriteLine(s.IsRunning); "
        "  Console.WriteLine(s.ElapsedMilliseconds >= 0); "
        "end Main; end C;"), "True\nTrue");
}

// The issue's repro shape: alias-qualified static factory (stdlib §8.1).
BEZEL_TEST(import_alias_stdlib_builtin_4698, alias_stopwatch_startnew_is_a_real_running_stopwatch) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class C public static method Main() "
        "  using var s := D.Stopwatch.StartNew(); "
        "  Console.WriteLine(s.IsRunning); "
        "  Console.WriteLine(s.ElapsedMilliseconds >= 0); "
        "end Main; end C;"), "True\nTrue");
}

// Alias in `new` and in a type annotation (stdlib §8.1: `constructor()`,
// `Start()`, `Stop()`): a never-started Stopwatch is not running, Start()
// makes it so, Stop() makes it not so.
BEZEL_TEST(import_alias_stdlib_builtin_4698, alias_stopwatch_new_and_annotation) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class C public static method Main() "
        "  using var s: D.Stopwatch := new D.Stopwatch(); "
        "  Console.WriteLine(s.IsRunning); "
        "  s.Start(); "
        "  Console.WriteLine(s.IsRunning); "
        "  s.Stop(); "
        "  Console.WriteLine(s.IsRunning); "
        "end Main; end C;"), "False\nTrue\nFalse");
}

// `D.Debug.WriteLine` (stdlib §8.2) must compile and run; stdlib §8.2 does not
// say which stream it writes, so the observable proof is that the statements
// AFTER it still run and the Stopwatch from the same alias is real.
BEZEL_TEST(import_alias_stdlib_builtin_4698, alias_debug_writeline_alongside_stopwatch) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class C public static method Main() "
        "  D.Debug.WriteLine(\"probe\"); "
        "  using var s := D.Stopwatch.StartNew(); "
        "  Console.WriteLine(s.IsRunning); "
        "end Main; end C;"), "True");
}

// The original #4698 shape (stdlib §6.1): `Th.CancellationTokenSource` in
// `new` AND as a type annotation. Cancel() then Token.IsCancellationRequested
// is true; before Cancel() it is false.
BEZEL_TEST(import_alias_stdlib_builtin_4698, alias_cancellationtokensource_new_and_annotation) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Threading as Th; "
        "class C public static method Main() "
        "  using var c: Th.CancellationTokenSource := new Th.CancellationTokenSource(); "
        "  Console.WriteLine(c.Token.IsCancellationRequested); "
        "  c.Cancel(); "
        "  Console.WriteLine(c.Token.IsCancellationRequested); "
        "end Main; end C;"), "False\nTrue");
}

// Control for the row above: the bare form.
BEZEL_TEST(import_alias_stdlib_builtin_4698, control_bare_cancellationtokensource) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Threading; "
        "class C public static method Main() "
        "  using var c: CancellationTokenSource := new CancellationTokenSource(); "
        "  Console.WriteLine(c.Token.IsCancellationRequested); "
        "  c.Cancel(); "
        "  Console.WriteLine(c.Token.IsCancellationRequested); "
        "end Main; end C;"), "False\nTrue");
}

// A System.Text type through an alias of an auto-imported module (spec §2.2
// has no exemption for always-available modules; stdlib §3.2 StringBuilder).
BEZEL_TEST(import_alias_stdlib_builtin_4698, alias_stringbuilder_of_auto_module) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Text as T; "
        "class C public static method Main() "
        "  var sb: T.StringBuilder := new T.StringBuilder(); "
        "  sb.Append(\"ab\").Append(\"cd\"); "
        "  Console.WriteLine(sb.ToString()); "
        "end Main; end C;"), "abcd");
}

// A System.Collections.Generic type through an alias (stdlib §4 Stack<T>:
// Push/Pop): LIFO order proves a real stack.
BEZEL_TEST(import_alias_stdlib_builtin_4698, alias_generic_stack_of_auto_module) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic as G; "
        "class C public static method Main() "
        "  var st: G.Stack<int> := new G.Stack<int>(); "
        "  st.Push(4); st.Push(9); "
        "  Console.WriteLine(st.Pop()); "
        "  Console.WriteLine(st.Pop()); "
        "end Main; end C;"), "9\n4");
}

// §2.7 rule 1: a local named like the alias is NOT the alias. The local
// `D` (a Stopwatch) has no member `Stopwatch`, so `D.Stopwatch` must not be
// rewritten into the built-in; the program is rejected (the member call on the
// local's own type), not silently re-bound to the module's Stopwatch.
BEZEL_TEST(import_alias_stdlib_builtin_4698, local_named_like_alias_is_not_rewritten) {
    auto r = bezel::test::runAot(
        "module M; import System.Diagnostics as D; "
        "class C public static method Main(): int "
        "  using var D := new Stopwatch(); "
        "  var x := D.Stopwatch.StartNew(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "on built-in type `Stopwatch` produced no value"));
}
