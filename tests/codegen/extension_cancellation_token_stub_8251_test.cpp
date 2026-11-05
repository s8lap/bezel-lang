// Issue #8251 — a USER `extension CancellationToken` never dispatched on a
// `CancellationToken` receiver whose type came from the built-in's own
// STATIC PROPERTY (`CancellationToken.None`) or from `CancellationTokenSource
// .Token`.
//
// THE DEFECT (measured on this lane's base, r30-c1 @ fb1184c7, spec
// revision v0.355):
//
//   b.bzl:8:23: `Twice` on built-in type `CancellationToken` produced no
//   value, and no dispatch arm emitted any code for this call.
//   `CancellationToken` has no user declaration, so its member surface is
//   exactly what the standard library documents for it: either `Twice` is not
//   on that surface (a misspelling), or it is documented but not implemented
//   by this compiler (#2457)
//
// WHAT THE SPEC REQUIRES.  Language spec §26.5 rule 2 (v0.355): "If the
// receiver's type declares no instance member of that name, the compiler
// searches the current module and all imported modules ... for extension
// blocks on `x`'s type, base types, or implemented interfaces".  stdlib §6.1
// declares `public record CancellationToken` with the instance surface
// `IsCancellationRequested` / `ThrowIfCancellationRequested` / `Register`
// (plus §8.1's universal members) — `Twice` is NOT on it, so rule 2 selects
// the user's block.  §26.2 admits records as extendable targets.
//
// THE MECHANISM.  A built-in reaches sema in TWO TypeRef shapes.  An
// ANNOTATION (`var t: CancellationToken := ...`) resolves to a
// DECLARATION-LESS Named ref; a static property or a built-in call result
// (`CancellationToken.None`, `cts.Token`) is stamped with the synthetic
// `builtinClassSym("CancellationToken")` TypeSymbol — a non-null
// declaration with no TypeDecl.  The §26.5 matcher
// (`extensionTargetMatchesReceiver`, src/sema/ExprTypecheck.cpp) reads a
// stub-typed receiver as a BUILT-IN only when its head's declared instance
// surface is one sema answers by name
// (`builtinStubHeadHasDeclaredSurface`, src/sema/ExprTypecheck_Exprs.cpp) —
// and `CancellationToken` was in no such table, so the stub was read as a
// USER type.  A user type and a built-in are never equated, so the extension
// head never matched and the extension was INVISIBLE: the call reached
// codegen with no callee at all.  The ANNOTATION shape was never affected
// (it is declaration-less already), which is exactly why the same extension
// worked there and not here.
//
// The fix transcribes stdlib §6.1's `CancellationToken` declaration block
// into the surface table, so the matcher reads the stub as the built-in it
// is — and, in the same stroke, makes §26.5 rule 1 commit for the members
// stdlib §6.1 DOES declare (a same-named extension of `IsCancellationRequested`
// is unreachable on a `CancellationToken` receiver).
//
// ORACLE: exact stdout (runAotChecked) for the dispatch rows; the compile
// rejection's diagnostic TEXT for the one row that must stay rejected.
//
// Cite: language spec §26.5 rule 2 / §26.2 / §8.1 (v0.355), stdlib §6.1
// (`CancellationToken`).

#include "support/Diagnostic.hpp"  // bezel::Severity
#include "support/aot_support.hpp"  // runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

// The extension block under test: a member stdlib §6.1 does NOT declare, so
// §26.5 rule 1 does not commit and rule 2 selects this block.
const char* kExt =
    "public extension CancellationToken "
    "public method Twice(): int => 2; "
    "end extension; ";

std::string prog(const std::string& body) {
    return std::string("module M; ") + kExt +
           "public class Program public static method Main(): int " + body +
           " return 0; end Main; end Program;";
}

// Is there an Error diagnostic carrying `keyword`?  (A compile REJECT is
// reported through `JitResult::diagnostics`; `runAot` / `runAotChecked` fill
// it only on a reject, which is exactly the case this helper asks about.)
bool hasError(const std::string& program, const std::string& keyword) {
    auto r = runAotChecked(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ---- the issue's own repro ------------------------------------------------
//
// `var t := CancellationToken.None;` — the receiver's type is the built-in's
// own static property, so the local's TypeRef is the `builtinClassSym` stub.
// Pre-fix: a compile error, so the program never ran.
BEZEL_TEST(extension_cancellation_token_stub_8251, cancellation_token_none_receiver_8251) {
    auto r = runAotChecked(prog(
        "  var t := CancellationToken.None; "
        "  Console.WriteLine(t.Twice()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---- the other stub shape: `CancellationTokenSource.Token` ----------------
//
// stdlib §6.1's `CancellationTokenSource.Token: CancellationToken` is the
// second source of the stub-typed receiver (the property's value is stamped
// with `builtinClassSym("CancellationToken")` too).
//
// The source OWNS the shared cancellation state, so the row loops it 100
// times under the checked oracle (ASan + LSan + dispose-exactly-once): a
// leak or a double free shows up only under repetition.  The row READS the
// extension's answer (it is not a constant body), so a fix that dispatched
// the wrong member would fail it.
BEZEL_TEST(extension_cancellation_token_stub_8251, cts_token_receiver_8251) {
    auto r = runAotChecked(prog(
        "  var n := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    using var cts := new CancellationTokenSource(); "
        "    if cts.Token.Twice() = 2 then "
        "      n := n + 1; "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(n); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

// ---- the receiver is actually PASSED to the extension ---------------------
//
// A block whose body ignores `this` cannot see a wrong receiver.  This one
// reads the receiver's OWN stdlib §6.1 surface through `this`, so the
// extension's `this` must be the token the caller holds — and the answer
// must be the token's own state, not a constant.
BEZEL_TEST(extension_cancellation_token_stub_8251, extension_body_reads_this_8251) {
    auto r = runAotChecked(
        "module M; "
        "public extension CancellationToken "
        "public method IsCancelled(): bool => this.IsCancellationRequested; "
        "end extension; "
        "public class Program public static method Main(): int "
        "  using var cts := new CancellationTokenSource(); "
        "  var t := cts.Token; "
        "  Console.WriteLine(t.IsCancelled()); "
        "  cts.Cancel(); "
        "  Console.WriteLine(t.IsCancelled()); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The row READS the token's own state: False before the source is
    // cancelled, True after.
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// ---- control: the ANNOTATION shape keeps working -------------------------
//
// `var t: CancellationToken := ...` was never affected (its TypeRef is
// declaration-less, so the matcher already read it as a built-in).  The fix
// must not narrow it.
BEZEL_TEST(extension_cancellation_token_stub_8251, annotated_receiver_still_works_8251) {
    auto r = runAotChecked(prog(
        "  var t: CancellationToken := CancellationToken.None; "
        "  var u := t; "
        "  Console.WriteLine(u.Twice()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---- control: another built-in's extension still works --------------------
//
// §26.5 rule 2 is not specific to `CancellationToken`: `Guid` (stdlib §2.6)
// is a built-in VALUE RECORD with the same stub shape, and its extension
// must keep dispatching.
BEZEL_TEST(extension_cancellation_token_stub_8251, guid_extension_still_works_8251) {
    auto r = runAotChecked(
        "module M; "
        "public extension Guid public method Twice(): int => 2; end extension; "
        "public class Program public static method Main(): int "
        "  var g := Guid.Empty; "
        "  Console.WriteLine(g.Twice()); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---- control: an UNDECLARED member on `CancellationToken` stays rejected --
//
// The fix admits the receiver's TYPE to the extension search; it must not
// admit a member stdlib §6.1 does not declare.  `t.Bogus()` is still
// refused — with the same located diagnostic as before the fix.
BEZEL_TEST(extension_cancellation_token_stub_8251, undeclared_member_still_rejected_8251) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; public class Program public static method Main(): int "
        "  var t := CancellationToken.None; "
        "  Console.WriteLine(t.Bogus()); "
        "  return 0; "
        "end Main; end Program;",
        "produced no value"));
}

// ---- control: §26.5 rule 1 still commits for a DECLARED member -----------
//
// stdlib §6.1 declares `IsCancellationRequested` on `CancellationToken`, so
// rule 1 commits the instance lookup for that name and an extension of the
// same name is UNREACHABLE on a `CancellationToken` receiver — the built-in
// answers.  (This is the flip the surface table buys: before it, the stub
// shape committed nothing.)
BEZEL_TEST(extension_cancellation_token_stub_8251, rule1_declared_member_wins_8251) {
    auto r = runAotChecked(
        "module M; "
        "public extension CancellationToken "
        "public method IsCancellationRequested(): bool => true; "
        "end extension; "
        "public class Program public static method Main(): int "
        "  var t := CancellationToken.None; "
        "  Console.WriteLine(t.IsCancellationRequested); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The BUILT-IN's answer (stdlib §6.1): `CancellationToken.None` has no
    // source and is never in the cancelled state — False, never the
    // extension's `true`.
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}
