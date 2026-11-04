// Spec-derived regression test for issue #7971 -- a `Task` PROPERTY called as
// a METHOD (`t.IsCompleted()`, `t.Result()`) was ACCEPTED, with an internal
// degradation warning instead of a diagnostic.
//
// THE SPEC.
//
// spec §30.3 (v0.355) declares, on `Task` / `Task<T>` / `OwningTask<T>`:
//
//   public readonly property IsCompleted: bool;
//   public readonly property IsFaulted: bool;
//   public readonly property IsCancelled: bool;
//   public readonly property Result: T;
//
// and exactly ONE method, `public method Wait();`. The three bool properties
// are declared on the base `Task` class and inherited by `Task<T>` and
// `OwningTask<T>`; `Result` is declared on `Task<T>` and `OwningTask<T>`.
//
// spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` -- of any arity, applicability, or
// accessibility -- instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver." So `t.IsCompleted()`
// COMMITS to the declared instance member -- it does not fall through to an
// extension, and it is not a free-standing call of an undeclared name.
//
// spec §27.1 (v0.350) admits only a FUNCTION-typed value as a callee ("a
// parameter, local, field, or property of function type"), and §27.7 (v0.350)
// is the compiler's "only function-typed values are callable" rule. A `bool`
// / `T` property is not function-typed, so the call spelling has no
// candidate: it is a compile error AT THE MEMBER.
//
// THE DEFECT -- MEASURED on the base tip (r30-c1 2fd1ed56, spec v0.355).
//
//   using var t := Task.FromResult(2);
//   var b := t.IsCompleted();        -> COMPILED, exit 0, with
//                                        "internal: lowerLocalDecl's
//                                         initializer expression failed to
//                                         lower to a value for this local's
//                                         slot (degraded to a zero-initialized
//                                         value) -- #2434"
//   var b := t.Result();             -> the same
//   var b := t.IsFaulted();          -> the same
//   var b := t.IsCancelled();        -> the same
//   var b := t.IsCompleted(1);       -> the same
//   var b := Task.Delay(1).IsCompleted();   -> the same (a bare `Task`)
//   var b := t.Result();  (OwningTask)     -> the same
//   t?.IsCompleted();                -> COMPILED, exit 0, with only a
//                                        "result unused" warning
//   t?.Result();                     -> the same
//
// WHY IT WAS ACCEPTED. In `ExprTC::resolveMemberAccess` the §30.3 property
// arms TYPED the expression and `return`ed, so the call spelling never
// reached the closed CALL surface that the same block applies a few lines
// below (`taskClosedCallMessage`, #7868 / #7869). The `?.` twin in
// `ExprTC::resolveNullCondMember` had the same early-return shape.
//
// THE ROWS. Every rejection row asserts the EXACT diagnostic text, at the
// call's own source line, and asserts that the program did not run and that
// the driver reported exactly ONE error (no cascade). The CONTROL rows are
// load-bearing for the over-reject direction: the bare property READS, the
// one real method (`Wait()`), the `?.` forms of both, the closed-call-surface
// diagnostic for a name the task does NOT declare, and a USER class's
// `Result` method / property must all still compile -- a fix that closed the
// whole task surface, or that rejected the name `Result` everywhere, would
// fail these rows.

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The body always sits on line 4.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Threading.Tasks;\n"  // 1
           "class C\n"                                                // 2
           "  public static method Main(): int\n"                     // 3
           "    " + body + "\n"                                       // 4
           "    return 0;\n"                                           // 5
           "  end Main;\n"                                             // 6
           "end C;\n";                                                // 7
}

// The same, with a same-named extension declared FIRST. The body sits on
// line 5.
std::string progWithExtension(const std::string& body) {
    return "module M; import System; import System.Threading.Tasks;\n"  // 1
           "extension Task public method IsCompleted(): bool => true; "
           "end extension;\n"                                            // 2
           "class C\n"                                                  // 3
           "  public static method Main(): int\n"                       // 4
           "    " + body + "\n"                                         // 5
           "    return 0;\n"                                             // 6
           "  end Main;\n"                                               // 7
           "end C;\n";                                                  // 8
}

// The same, with a `D` class to own. The body sits on line 5.
std::string progWithOwned(const std::string& body) {
    return "module M; import System; import System.Threading.Tasks;\n"  // 1
           "class D public v: int := 1; end D;\n"                       // 2
           "class C\n"                                                  // 3
           "  public static method Main(): int\n"                       // 4
           "    " + body + "\n"                                         // 5
           "    return 0;\n"                                             // 6
           "  end Main;\n"                                               // 7
           "end C;\n";                                                  // 8
}

// The same, over a USER class that declares its own `Result` -- a METHOD in
// one program and a PROPERTY in the other. The body sits on line 5.
std::string progWithUserClass(const std::string& decl, const std::string& body) {
    return "module M; import System;\n"                                // 1
           "class Foo\n"                                                // 2
           "  " + decl + "\n"                                           // 3
           "end Foo;\n"                                                 // 4
           "class C\n"                                                  // 5
           "  public static method Main(): int\n"                       // 6
           "    " + body + "\n"                                         // 7
           "    return 0;\n"                                             // 8
           "  end Main;\n"                                               // 9
           "end C;\n";                                                  // 10
}

// One `<path>:<line>:<col>: <text>` line of the driver's captured stderr.
struct DiagLine {
    std::uint32_t line = 0;
    std::string text;
};

// The AOT oracle packs the driver's ENTIRE captured stderr into ONE
// `Diagnostic` (a genuine compile reject), whose message is the driver's
// `<path>:<line>:<col>: <text>` diagnostic lines, one per line. This splits
// that blob back into its lines.
std::vector<DiagLine> diagnosticLines(const JitResult& r) {
    std::vector<DiagLine> out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::istringstream in(d.message);
        std::string raw;
        while (std::getline(in, raw)) {
            const std::size_t c1 = raw.find(':');
            if (c1 == std::string::npos) continue;
            const std::size_t c2 = raw.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            const std::size_t c3 = raw.find(':', c2 + 1);
            if (c3 == std::string::npos) continue;
            DiagLine dl;
            dl.line = static_cast<std::uint32_t>(std::strtoul(
                raw.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10));
            dl.text = raw.substr(c3 + 1);
            if (!dl.text.empty() && dl.text[0] == ' ') dl.text.erase(0, 1);
            out.push_back(dl);
        }
    }
    return out;
}

// A warning / note line carries its severity as a prefix; an error line does
// not.
bool isErrorLine(const DiagLine& dl) {
    return dl.text.rfind("warning: ", 0) != 0 && dl.text.rfind("note: ", 0) != 0;
}

// The EXACT text of the error diagnostic the driver reported at `line`.
std::string errorTextAtLine(const JitResult& r, std::uint32_t line) {
    for (const auto& dl : diagnosticLines(r))
        if (dl.line == line && isErrorLine(dl)) return dl.text;
    return std::string();
}

// How many ERROR diagnostics the driver reported in all.
std::size_t errorLineCount(const JitResult& r) {
    std::size_t n = 0;
    for (const auto& dl : diagnosticLines(r))
        if (isErrorLine(dl)) ++n;
    return n;
}

// The rejection oracle: the program did NOT run, the driver rejected it at a
// REAL source location (never a crash / signal / timeout), the diagnostic on
// `line` reads EXACTLY as expected, and it is the ONLY error (a fix that
// produced a cascade would fail here).
void expectRejectedWith(const std::string& program, std::uint32_t line,
                        const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorTextAtLine(r, line), expected);
    BEZEL_EXPECT_EQ(errorLineCount(r), std::size_t{1});
}

// The EXACT diagnostics the fix introduces. One per property shape.
std::string boolPropertyCallMessage(const std::string& member) {
    return "`" + member + "` is a property of `Task`, not a method "
           "(§30.3: `public readonly property " + member +
           ": bool;`), so this call has no candidate: the lookup commits to "
           "the instance member (§26.5 rule 1), and only a function-typed "
           "property can be called (§27.1, §27.7) — read it without "
           "parentheses, `." + member + "`";
}

const char* kResultPropertyCallMessage =
    "`Result` is a property of `Task<T>` / `OwningTask<T>`, not a method "
    "(§30.3: `public readonly property Result: T;`), so this call has no "
    "candidate: the lookup commits to the instance member (§26.5 rule 1), "
    "and only a function-typed property can be called (§27.1, §27.7) — "
    "read it without parentheses, `.Result`";

// The closed CALL surface's own diagnostic -- for a name the task does NOT
// declare. It must stay DISTINCT from the property-call diagnostic above.
std::string closedCallMessage(const std::string& member) {
    return "`Task` has no method `" + member +
           "` (§30.3): its members are the properties IsCompleted, "
           "IsFaulted, IsCancelled and Result, and the method Wait()";
}

// A program that compiles, runs, and prints `out`.
void expectStdout(const std::string& program, const std::string& out) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A program that compiles and runs (its printed value is not the subject of
// the row).
void expectRuns(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

}  // namespace

// ===========================================================================
// THE DEFECT: the CALL spelling of a §30.3 task property.
// ===========================================================================

// The issue's own repro: `t.IsCompleted()`.
BEZEL_TEST(task_property_call_7971,
           is_completed_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t := Task.FromResult(2); var b := t.IsCompleted();"),
        4, boolPropertyCallMessage("IsCompleted"));
}

// The other two bool properties of the base `Task` class.
BEZEL_TEST(task_property_call_7971,
           is_faulted_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t := Task.FromResult(2); var b := t.IsFaulted();"), 4,
        boolPropertyCallMessage("IsFaulted"));
}

BEZEL_TEST(task_property_call_7971,
           is_cancelled_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t := Task.FromResult(2); var b := t.IsCancelled();"),
        4, boolPropertyCallMessage("IsCancelled"));
}

// `Result` -- declared on `Task<T>` / `OwningTask<T>`.
BEZEL_TEST(task_property_call_7971, result_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t := Task.FromResult(2); var b := t.Result();"), 4,
        kResultPropertyCallMessage);
}

// The `?.` twin of the same call.
BEZEL_TEST(task_property_call_7971,
           null_conditional_is_completed_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t0 := Task.FromResult(2); var t: Task<int>? := t0; "
             "var b := t?.IsCompleted();"),
        4, boolPropertyCallMessage("IsCompleted"));
}

BEZEL_TEST(task_property_call_7971,
           null_conditional_result_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t0 := Task.FromResult(2); var t: Task<int>? := t0; "
             "var b := t?.Result();"),
        4, kResultPropertyCallMessage);
}

// An ARGUMENT does not turn the property into a method: the call still has no
// candidate.
BEZEL_TEST(task_property_call_7971,
           is_completed_call_with_an_argument_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t := Task.FromResult(2); var b := t.IsCompleted(1);"),
        4, boolPropertyCallMessage("IsCompleted"));
}

// A bare `Task` (no type argument) declares the three bool properties too.
BEZEL_TEST(task_property_call_7971,
           non_generic_task_is_completed_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        prog("using var t := Task.Delay(1); var b := t.IsCompleted();"), 4,
        boolPropertyCallMessage("IsCompleted"));
}

// `OwningTask<T>` declares `Result` as well (spec §30.3).
BEZEL_TEST(task_property_call_7971,
           owning_task_result_call_is_rejected_spec_30_3) {
    expectRejectedWith(
        progWithOwned("using var t := Task.FromOwnedResult(new D()); "
                      "var b := t.Result();"),
        5, kResultPropertyCallMessage);
}

// §26.5 rule 1: the declared instance member COMMITS the lookup, so a
// same-named extension is never consulted -- the extension must not rescue
// the call.
BEZEL_TEST(task_property_call_7971,
           an_extension_does_not_rescue_the_property_call_spec_26_5_rule_1) {
    expectRejectedWith(
        progWithExtension("using var t := Task.FromResult(2); "
                          "var b := t.IsCompleted();"),
        5, boolPropertyCallMessage("IsCompleted"));
}

// ===========================================================================
// CONTROLS: what must still compile.
// ===========================================================================

// The bare READS -- the spelling §30.3 declares -- still work, and still
// read the task's state.
BEZEL_TEST(task_property_call_7971,
           property_reads_still_compile_spec_30_3) {
    expectStdout(prog("using var t := Task.FromResult(2); "
                      "Console.WriteLine(t.IsCompleted.ToString()); "
                      "Console.WriteLine(t.IsFaulted.ToString()); "
                      "Console.WriteLine(t.IsCancelled.ToString()); "
                      "Console.WriteLine(t.Result.ToString());"),
                 "True\nFalse\nFalse\n2\n");
}

// The ONE method §30.3 declares is still callable, in both spellings.
BEZEL_TEST(task_property_call_7971, wait_call_still_compiles_spec_30_3) {
    expectRuns(prog("using var t := Task.FromResult(2); t.Wait();"));
}

BEZEL_TEST(task_property_call_7971,
           null_conditional_wait_call_still_compiles_spec_30_3) {
    expectRuns(prog("using var t0 := Task.FromResult(2); "
                    "var t: Task<int>? := t0; t?.Wait();"));
}

// The `?.` READ of a task property is still accepted -- the fix rejects the
// CALL spelling only. (The printed value is not the subject of this row: a
// nullable `bool` in a local slot is a separate, pre-existing codegen gap.)
BEZEL_TEST(task_property_call_7971,
           null_conditional_property_read_still_compiles_spec_30_3) {
    expectRuns(prog("using var t0 := Task.FromResult(2); "
                    "var t: Task<int>? := t0; "
                    "if t?.IsCompleted ?? false then "
                    "Console.WriteLine(\"yes\"); end;"));
}

// A name the task does NOT declare keeps the CLOSED CALL SURFACE's own
// diagnostic -- distinct from the property-call diagnostic, and not the
// over-broad "no member" text.
BEZEL_TEST(task_property_call_7971,
           an_undeclared_member_keeps_the_closed_call_surface_diagnostic) {
    expectRejectedWith(
        prog("using var t := Task.FromResult(2); t.Bogus();"), 4,
        closedCallMessage("Bogus"));
}

// A bare `Task` has no `Result` to read (it is declared on `Task<T>` /
// `OwningTask<T>`), so that call keeps the closed call surface's diagnostic.
BEZEL_TEST(task_property_call_7971,
           non_generic_task_result_keeps_the_closed_call_surface_diagnostic) {
    expectRejectedWith(
        prog("using var t := Task.Delay(1); var b := t.Result();"), 4,
        closedCallMessage("Result"));
}

// A USER class's `Result` METHOD is still callable: the fix is about the
// §30.3 TASK properties, not about the name.
BEZEL_TEST(task_property_call_7971,
           a_user_class_result_method_is_still_callable) {
    expectStdout(
        progWithUserClass(
            "private result: int := 7; "
            "public method Result(): int return this.result; end Result;",
            "var foo := new Foo(); Console.WriteLine(foo.Result().ToString());"),
        "7\n");
}

// A USER class's `Result` PROPERTY is still readable, and still a property.
BEZEL_TEST(task_property_call_7971,
           a_user_class_result_property_is_still_readable) {
    expectStdout(
        progWithUserClass("public property Result: int := 7;",
                          "var foo := new Foo(); "
                          "Console.WriteLine(foo.Result.ToString());"),
        "7\n");
}

// ===========================================================================
// FIX-FORWARD: a FUNCTION-typed `Result` IS a legal callee.
//
// spec §27.1 (v0.355) admits as a callee "a function-typed value -- a
// parameter, local, field, or property of function type, or any expression of
// one", and §30.3 (v0.355) declares `public readonly property Result: T;`. At
// `T` a function type, `t.Result()` reads the property and calls the function
// it holds -- exactly as a user class's function-typed property `h.Cb()` is
// called. The first cut of #7971 (e5ad8c2f) refused every `t.Result()`; these
// rows compiled and ran correctly on its base (2fd1ed56) and were refused at
// e5ad8c2f.
// ===========================================================================

// `Task<() -> int>`: read `Result`, call it.
BEZEL_TEST(task_property_call_7971,
           function_typed_result_call_runs_spec_27_1_spec_30_3) {
    expectStdout(prog("using var t := Task.FromResult<() -> int>(lambda() => 7); "
                      "var v := t.Result(); "
                      "Console.WriteLine(v.ToString());"),
                 "7\n");
}

// The explicitly parenthesised "read, then call" spelling.
BEZEL_TEST(task_property_call_7971,
           parenthesised_function_typed_result_call_runs_spec_27_1_spec_30_3) {
    expectStdout(prog("using var t := Task.FromResult<() -> int>(lambda() => 7); "
                      "var v := (t.Result)(); "
                      "Console.WriteLine(v.ToString());"),
                 "7\n");
}

// A function type WITH a parameter: the argument is the function's, not the
// property's.
BEZEL_TEST(task_property_call_7971,
           function_typed_result_call_with_an_argument_runs_spec_27_1_spec_30_3) {
    expectStdout(
        prog("using var t := Task.FromResult<(int) -> int>("
             "lambda(x: int) => x + 1); "
             "Console.WriteLine(t.Result(41).ToString());"),
        "42\n");
}

// An `async` method's `Task<() -> int>`, awaited in one async body and called
// through `Result` in the synchronous `Main`.
BEZEL_TEST(task_property_call_7971,
           async_function_typed_result_call_in_sync_main_runs_spec_27_1_spec_30_3) {
    expectStdout(
        "module M; import System; import System.Threading.Tasks;\n"
        "class C\n"
        "  public static async method Mk(): Task<() -> int> "
        "return lambda() => 8; end Mk;\n"
        "  public static async method Run(): Task<int>\n"
        "    using var t := Mk();\n"
        "    var f := await t;\n"
        "    return f();\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    using var t := Run();\n"
        "    Console.WriteLine(t.Result.ToString());\n"
        "    using var u := Mk();\n"
        "    Console.WriteLine(u.Result().ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "8\n8\n");
}

// The `?.` twin: §7.5 (v0.355) types a suffix to the right of `?.` at the
// value type, so over `Task<() -> int>?` the `( )` suffix calls the function
// `Result` holds -- a legal call, whose result is nullable. A USER class's
// function-typed property `h?.Cb()` is accepted on the same terms. Only the
// ACCEPTANCE is the subject of this row: the printed value is a pre-existing,
// shared codegen gap (#2434 degrades the `?.`-call initializer for the user
// class's twin too), not this fix's.
BEZEL_TEST(task_property_call_7971,
           null_conditional_function_typed_result_call_is_accepted_spec_7_5_spec_27_1) {
    expectRuns(prog("using var t0 := Task.FromResult<() -> int>(lambda() => 7); "
                    "var t: Task<() -> int>? := t0; "
                    "var v := t?.Result(); "
                    "Console.WriteLine((v ?? 0).ToString());"));
}

// A NULLABLE function-typed `Result` is invoked with the null-conditional call
// `?(...)` (§7.6, v0.355), and runs.
BEZEL_TEST(task_property_call_7971,
           nullable_function_typed_result_null_conditional_call_runs_spec_7_6) {
    expectStdout(
        prog("using var t := Task.FromResult<(() -> int)?>(lambda() => 5); "
             "var v := t.Result?(); "
             "Console.WriteLine((v ?? 0).ToString());"),
        "5\n");
}

// The `.Result` blocking wait in ASYNC code is still refused for a
// function-typed T (§30.3: "use `await t`"): admitting the call spelling must
// not drop the async rule, and the property-call diagnostic must not fire in
// its place.
BEZEL_TEST(task_property_call_7971,
           async_function_typed_result_call_keeps_the_await_diagnostic_spec_30_3) {
    expectRejectedWith(
        "module M; import System; import System.Threading.Tasks;\n"
        "class C\n"
        "  public static async method A(): Task<int>\n"
        "    using var t := Task.FromResult<() -> int>(lambda() => 7);\n"
        "    return t.Result();\n"
        "  end A;\n"
        "  public static method Main(): int\n"
        "    using var u := A();\n"
        "    Console.WriteLine(u.Result.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        5,
        "use `await t` — blocking waits are not available in async code "
        "(§30.3)");
}
