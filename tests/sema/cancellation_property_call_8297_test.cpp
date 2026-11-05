// Issue #8297 -- a PROPERTY called with ARGUMENTS on a built-in value record
// receiver whose type came from a built-in CLASS STUB was silently accepted:
//
//   var t := CancellationToken.None;
//   Console.WriteLine(t.IsCancellationRequested(1));
//
// compiled with two `warning: internal` lines, the argument "failed to lower",
// and the whole statement dropped (exit 0, nothing printed). The same call on
// an ANNOTATED receiver (`var t: CancellationToken := ...`) was already
// refused, and so was `string.Length(1)`.
//
// THE SPEC. Spec §26.5 rule 1 (v0.355): an instance member named `Foo`, of any
// arity, commits the instance lookup; if the committed lookup then fails (no
// applicable overload, §22.5 / §22.6), the call is a compile error citing the
// instance member. A property is not a function, so it has no argument
// binding: `t.IsCancellationRequested(1)` has no candidate. Spec §27.1 admits
// only a function-typed value as a callee.
//
// THE MECHANISM. The value-record arity arm (`resolveMemberAccessWithRule1`,
// src/sema/ExprTypecheck_Exprs.cpp) read the receiver's head only for the
// DECLARATION-LESS TypeRef shape, so a stub receiver (non-null synthetic
// `builtinClassSym` declaration) was skipped. The fix resolves the head with
// `valueRecordHeadOf` for that shape, the same resolver the `?.` twin uses.
// The stub shape is exercised by `CancellationToken.None`, `TimeSpan.Zero`,
// `DateTime.UtcNow`.
//
// ORACLE. Rejection rows use `runAot`: the program does not run, the rejection
// is at a real source location, and the error names the instance member on the
// call's own line. Control rows use `runAotChecked` and assert exact stdout.

#include "support/Diagnostic.hpp"   // bezel::Severity
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"  // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Does the driver report an Error diagnostic that carries every `needle`?
// (`runAot` fills `diagnostics` only on a reject, which is what this asks.)
bool hasErrorWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// The program with `body` on line 4 (`Main` opens on line 3).
std::string prog(const std::string& body) {
    return "module M;\n"                                      // 1
           "public class Program\n"                           // 2
           "  public static method Main(): int\n"             // 3
           "    " + body + "\n"                               // 4
           "    return 0;\n"                                  // 5
           "  end Main;\n"                                    // 6
           "end Program;\n";                                  // 7
}

}  // namespace

// ---- the issue's own repro: CancellationToken stub receiver ----------------
//
// Pre-fix: accepted, exit 0. Post-fix: rejected at the call (line 4), with the
// instance member named and the supplied argument count given.
BEZEL_TEST(cancellation_property_call_8297, cancellation_token_stub_property_with_args_8297) {
    auto r = runAot(prog(
        "var t := CancellationToken.None; "
        "Console.WriteLine(t.IsCancellationRequested(1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    const std::string p = prog(
        "var t := CancellationToken.None; "
        "Console.WriteLine(t.IsCancellationRequested(1));");
    BEZEL_EXPECT_TRUE(hasErrorWith(p,
        "`IsCancellationRequested` on `CancellationToken` is an instance member"));
    BEZEL_EXPECT_TRUE(hasErrorWith(p,
        "The instance member takes no arguments, but this call supplies 1."));
}

// The same spelling on the OTHER stub shape: `CancellationTokenSource.Token`.
BEZEL_TEST(cancellation_property_call_8297, cts_token_stub_property_with_args_8297) {
    const std::string p = prog(
        "using var cts := new CancellationTokenSource(); "
        "Console.WriteLine(cts.Token.IsCancellationRequested(1));");
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorWith(p, "takes no arguments, but this call supplies 1"));
}

// Another value record with a stub receiver: `DateTime.UtcNow` is a stub, and
// `Year` is a declared arity-0 property of `DateTime` (stdlib §2.5).
BEZEL_TEST(cancellation_property_call_8297, datetime_stub_property_with_args_8297) {
    const std::string p = prog(
        "var d := DateTime.UtcNow; "
        "Console.WriteLine(d.Year(1));");
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorWith(p, "`Year` on `DateTime` is an instance member"));
}

// TimeSpan stub: `TimeSpan.Zero` is a stub, and `Days` is a declared property.
BEZEL_TEST(cancellation_property_call_8297, timespan_stub_property_with_args_8297) {
    const std::string p = prog(
        "var z := TimeSpan.Zero; "
        "Console.WriteLine(z.Days(1));");
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorWith(p, "`Days` on `TimeSpan` is an instance member"));
}

// ---- controls: what stays legal ---------------------------------------------

// The property READ on the same stub receiver still answers (stdlib §6.1:
// `CancellationToken.None` has no source and is never cancelled).
BEZEL_TEST(cancellation_property_call_8297, cancellation_token_property_read_still_answers_8297) {
    auto r = runAotChecked(prog(
        "var t := CancellationToken.None; "
        "Console.WriteLine(t.IsCancellationRequested);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// A declared METHOD with arguments on a stub receiver is unaffected: `Guid`
// stub (`Guid.Empty`), `CompareTo` is a declared method (stdlib §2.6) and it
// takes one argument. The row reads the answer, so a wrong dispatch would show.
BEZEL_TEST(cancellation_property_call_8297, guid_stub_method_with_args_still_runs_8297) {
    auto r = runAotChecked(prog(
        "var g := Guid.Empty; "
        "Console.WriteLine(g.CompareTo(g));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// A declared METHOD on the same CancellationToken stub receiver with its own
// arguments: `Register` takes one callback (stdlib §6.1). Only the shape of the
// refusal is asserted here, not the dispatch: this row must not be refused as a
// property call.
BEZEL_TEST(cancellation_property_call_8297, cancellation_token_method_with_args_not_property_refusal_8297) {
    const std::string p = prog(
        "var t := CancellationToken.None; "
        "t.Register(() -> Console.WriteLine(\"x\"));");
    BEZEL_EXPECT_FALSE(hasErrorWith(p, "is a property of"));
    BEZEL_EXPECT_FALSE(hasErrorWith(p, "takes no arguments, but this call supplies"));
}
