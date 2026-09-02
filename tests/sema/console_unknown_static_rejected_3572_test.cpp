// fix #3572 — the surviving half of the #1037 missing-reject-fallback family.
// `Console` is the one compiler-known built-in static class whose
// member-access typing arm (ExprTypecheck_Calls.cpp, `cls == "Console"`) has
// NO `rejectBuiltinStatic` fallback, so any member name on it was silently
// type-checked, lowered to nothing, and ran. The issue's reproducer:
//
//     module P; class C public static method Main(): int
//       Console.Bogus();
//       Console.Clear();     // REMOVED from stdlib §2.1 at v0.284
//       return 7;
//     end Main; end C;
//
// → compiles, runs, exits 7. The fix mirrors the Convert / Math / BitConverter
// pattern (#1037, #1038, #3344): split per class, drop an accept-list of the
// exact members the codegen arm (`LlvmBackend_Calls.cpp`'s `tryLowerConsoleCall`
// + the Read/ReadLine/ReadKey/ReadKeyOrNull paths in
// `LlvmBackend_Calls2.cpp`) lowers, and reject anything outside that list with
// the class's own stdlib-§ diagnostic. §2.1 names `Console`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A bare `module M; class C ... end C;` shell — the Console members are
// compiler-intrinsic, so no imports or hierarchy is required.
std::string consoleProg(const std::string& mainBody) {
    return "module M; class C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// True iff at least one error diagnostic carries the §2.1 no-such-static
// wording for the qualified name `Console.<member>`.
bool rejectedAsUnknownStatic(const bezel::test::JitResult& r,
                             const std::string& member) {
    const std::string needle = "'Console' has no static method or property '"
                             + member + "' (§2.1)";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error && d.message.find(needle)
                != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── REJECT — a fully-unknown name was the original #1037 shape. ─────────────
// Before the fix `Console.Bogus()` accepted silently and lowered to a no-op
// (compiled and ran).
BEZEL_TEST(sema_console_unknown_3572, reject_unknown_static_bogus) {
    auto r = runAot(consoleProg(
        "    Console.Bogus();\n"
        "    return 7;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsUnknownStatic(r, "Bogus"));
}

// ── REJECT — `Console.Clear()` was REMOVED from stdlib §2.1 at v0.284 ───────
// (proposal 0121 R3 / issue 3559 lists it as zero-hits-repo-wide). It is a
// different fact from a member that never existed, but for #3572's scope
// (the family of "the built-in-class path has no reject fallback") the right
// diagnostic is the unknown-static one — Console has no v0.284 specific
// accept-list with its own removal phrasing, unlike Math (§2.2 carries the
// transcendental block). The precedence rule (v0.286) is per-class.
// Without the fix: silently accepted, ran, exited 7.
BEZEL_TEST(sema_console_unknown_3572, reject_removed_v0284_clear) {
    auto r = runAot(consoleProg(
        "    Console.Clear();\n"
        "    return 7;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsUnknownStatic(r, "Clear"));
}

// ── REJECT — the original issue's combined repro (Bogus + Clear + return). ──
// This is the exact `module P; class C … return 7` the issue filed — kept
// side-by-side with the splits, so a future refactor that accepts the joined
// form but rejects either half alone is still caught.
BEZEL_TEST(sema_console_unknown_3572, reject_combined_issue_repro) {
    auto r = runAot(
        "module P; class C public static method Main(): int "
        "  Console.Bogus(); "
        "  Console.Clear(); "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsUnknownStatic(r, "Bogus"));
    BEZEL_EXPECT_TRUE(rejectedAsUnknownStatic(r, "Clear"));
}

// ── ACCEPT-CONTROLS — the live risk of any reject-with-fallback is silent
// over-rejection of members that STAYED. Every accept-list entry must
// still compile AND lower to something that runs.
namespace {

// The `WriteLine(string)` overload — the most-exercised Console member in the
// suite (2724+ uses), pinned here so a fix that rejects it on the wrong axis
// is caught as a real regression instead of a "Console.WriteLine broken"
// the next lane reports.
BEZEL_TEST(sema_console_unknown_3572, accept_writeline_string_still_works) {
    auto r = runAot(consoleProg(
        "    Console.WriteLine(\"hi\");\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The arity-0 `WriteLine()` overload — codegen's tryLowerConsoleCall treats
// `WriteLine` and `WriteLine(value)` differently (lines 167, 196). Both must
// remain accepted.
BEZEL_TEST(sema_console_unknown_3572, accept_writeline_noarg_still_works) {
    auto r = runAot(consoleProg(
        "    Console.WriteLine();\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The `WriteLine(Object)` overload — the §2.1 `Object` overload runs through
// the §3/#605 permissive surface (`o.Anything()`). Must remain accepted.
BEZEL_TEST(sema_console_unknown_3572, accept_writeline_object_still_works) {
    auto r = runAot(consoleProg(
        "    var o: Object := \"hi\";\n"
        "    Console.WriteLine(o);\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// `Write(value)` — required to stay accepted by the arity check at
// `ExprTypecheck_Calls.cpp:3750` (the `nargs == 1` branch for `isWrite`).
// Returns 7 from a `Write("hi")` to prove the program ran end-to-end.
BEZEL_TEST(sema_console_unknown_3572, accept_write_string_still_works) {
    auto r = runAot(consoleProg(
        "    Console.Write(\"hi\");\n"
        "    return 7;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// `ReadLine(): string?` — nullable return value typed in the Console block
// already; this row pins the typing path was not disturbed.
BEZEL_TEST(sema_console_unknown_3572, accept_readline_still_compiles) {
    auto r = runAot(consoleProg(
        "    var s := Console.ReadLine();\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// `ReadKey(): char` — non-nullable return value typed in the Console block
// already; same shape as `ReadLine`.
BEZEL_TEST(sema_console_unknown_3572, accept_readkey_still_compiles) {
    auto r = runAot(consoleProg(
        "    var c := Console.ReadKey();\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// `ReadKeyOrNull(): char?` (v0.231) — the §1 *OrNull tier beside ReadKey.
// Must remain accepted as a Console method. (The compiler intentionally
// leaves the ma untyped today, so the row just asserts "it compiles and
// does not regress to a reject".)
BEZEL_TEST(sema_console_unknown_3572, accept_readkeyornull_still_compiles) {
    auto r = runAot(consoleProg(
        "    var c: char? := Console.ReadKeyOrNull();\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The `Console.Error.WriteLine` facet is reached via a different member-access
// shape (property read on `Console.Error` then method call on TextWriter),
// NOT through this accept-list, so it must keep working regardless of the fix.
BEZEL_TEST(sema_console_unknown_3572, accept_console_error_writeline_still_works) {
    auto r = runAot(consoleProg(
        "    Console.Error.WriteLine(\"err\");\n"
        "    return 9;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

}  // namespace
