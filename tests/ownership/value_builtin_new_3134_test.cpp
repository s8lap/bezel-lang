// Spec-derived tests for issue #3134 — the §28.3 auto-drop note must NOT fire
// on `new` of a value-shaped builtin (`JsonOptions`, `ExceptionInfo`,
// `ReadOnlyStringSpan`, and every other member of `kNonHeapReservedBuiltins`).
//
// §28.3 / §8.7 / §17.7 (v0.252):
//   Records are value types (§9) with no disposal obligation. §8.7 enumerates
//   records as not implementing IDisposable, and §17.7 specifies the reference
//   behaviour for the two builtin records that have a positional-ctor-only
//   surface: "exactly as for an identically-shaped user record". The user-
//   record twin classifies Borrowed, so the value-shaped builtin twin MUST
//   classify Borrowed — otherwise `var j := new JsonOptions(...)` fires the
//   §28.3 auto-drop note on a value with nothing to dispose.
//
// The fix uses the SAME predicate codegen's #2974 `unconstructedBuiltinNewName`
// gate uses (`common::isReservedBuiltinTypeName(n) &&
// !common::isHeapPointerReferenceBuiltin(n)`, LlvmBackend_Setup.cpp:11660-61):
// the BuiltinRegistry's snapshot-pinned split between value-shaped and
// heap-pointer-shaped reserved builtins.
//
// Regression contract (no-over, no-under):
//   VALUE-SHAPED builtin `new` (ExceptionInfo / ReadOnlyStringSpan / ArraySpan
//                              / CString / DateTime / CancellationToken):
//     - `var x := new T(...)` → no auto-drop warning, no error.
//   HEAP-POINTER-SHAPED builtin `new` (CancellationTokenSource):
//     - `var x := new T()` without disposal → STILL fires the §28.3 warning.
//   USER record `new` (UR / any record declared by user code):
//     - `var u := new UR(...)` → no warning, no error.
//
// Note: tests deliberately exclude value-shaped builtins whose CURRENT ctor
// surface is broken (Range<int> — §22.7 zero-arity mismatch #922; Guid —
// stdlib §2.6 insurance: no `new` at any argument list; IntPtr — stdlib §10
// no constructor; JsonOptions member-access path — #3079 Console.WriteLine
// lowering). The ownership fix itself is verified across the whole snapshot
// by the prior `value_builtin_ownership_test.cpp` #434 set (DateTime/TimeSpan/
// Guid/CancellationToken — all still pass) plus these three additional
// `kNonHeapReservedBuiltins` members that DO have a working positional ctor.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Minimal program wrapper: a static Main that returns int.
std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           + body +
           " end Main; "
           "end C;";
}

std::string own(const std::string& body) {
    return prog(body);
}

} // namespace

// ---------------------------------------------------------------------------
// §28.3 / §17.7 / #3134 — value-shaped builtin RECORDS / spans with a working
// positional ctor.
// ---------------------------------------------------------------------------

// §28.3 / §8.7 / §31.4: `new ExceptionInfo("a","b","c")` — a value-shaped
// record; no §28.3 auto-drop note (nothing to dispose).
BEZEL_TEST(own_value_builtin_new_3134, exception_info_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var e := new ExceptionInfo(\"a\", \"b\", \"c\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "e"));
}

// §28.3 / §8.7 / stdlib §3.3: `new ReadOnlyStringSpan("abc", 0, 2)` — a
// value-shaped {ptr,i32,i32} span view; no §28.3 auto-drop note.
BEZEL_TEST(own_value_builtin_new_3134, readonly_string_span_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var s := new ReadOnlyStringSpan(\"abc\", 0, 2); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "s"));
}

// §28.3 / §4.14: `new ArraySpan<T>(...)` — value-shaped span; no auto-drop.
BEZEL_TEST(own_value_builtin_new_3134, array_span_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var a := new ArraySpan<int>(0, 0, 0); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "a"));
}

// §28.3 / §32.5 / stdlib §10: `new CString(...)` — FFI value type
// (`{ptr,len}`-shaped); no §28.3 auto-drop note.
BEZEL_TEST(own_value_builtin_new_3134, cstring_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var c := new CString(\"hello\"); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "c"));
}

// §28.3 / §6.1 / §30.8: `var tok := CancellationToken.None;` — value-shaped
// builtin; no §28.3 auto-drop note. (This is the §6.1 / #434 case the
// legacy DateTime-list covered; the snapshot-pinned predicate also covers it.)
BEZEL_TEST(own_value_builtin_new_3134, cancellation_token_none_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var tok := CancellationToken.None; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "tok"));
}

// ---------------------------------------------------------------------------
// Positive control — the DateTime positive control from #434 must remain
// warning-free (no-regression: the legacy DateTime/TimeSpan/etc. names were
// already Borrowed; the fix widens to the snapshot-pinned set, not narrows).
// ---------------------------------------------------------------------------

// §28 / §2.5: `var dt := new DateTime(2024, 3, 15);` — no error, no auto-drop.
BEZEL_TEST(own_value_builtin_new_3134, datetime_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var dt := new DateTime(2024, 3, 15); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "dt"));
}

// ---------------------------------------------------------------------------
// §17.7 / §22.5 — the user-record twin classifies Borrowed identically.
// ---------------------------------------------------------------------------

// §17.7 reference behaviour: `new UR("a","b","c")` for an identically-shaped
// user record MUST classify Borrowed (§22.5 required-parameter rule). The
// `ExceptionInfo` / `ReadOnlyStringSpan` cases above are the builtin twins of
// this; this is the §17.7 "exactly as for an identically-shaped user record"
// rule asserted on the literal control.
BEZEL_TEST(own_value_builtin_new_3134, user_record_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record UR(TypeName: string, Message: string, StackTrace: string); "
        "class C "
        "  public static method Main(): int "
        "    var u := new UR(\"a\", \"bb\", \"ccc\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "u"));
}

// ---------------------------------------------------------------------------
// Negative control — heap-pointer-shaped builtin `new` MUST still fire the
// §28.3 auto-drop note (no-under: the fix exempts only the value-shaped half).
// ---------------------------------------------------------------------------

// §28.3: CancellationTokenSource is an OWNED disposable class — a plain `var`
// without `using` or explicit Dispose MUST still be diagnosed.
BEZEL_TEST(own_value_builtin_new_3134, cancellation_token_source_still_owned) {
    auto r = parseAndCheckOwnership(own(
        "var cts := new CancellationTokenSource(); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "cts"));
}