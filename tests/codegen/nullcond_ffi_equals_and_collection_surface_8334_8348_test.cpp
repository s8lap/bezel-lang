// Regression tests for the `?.`-SPELLING family — issues #8334 and #8348.
//
// Both are the SAME root cause: the `?.` (null-conditional) spelling takes a
// path that SKIPS a gate the `.` spelling obeys.
//
// ── #8334 ── the `?.` spelling of `Equals` on an FFI-buffer receiver (§8.1 /
//    §8.6 / §8.3 / §28.9, v0.355).
//
// §8.1 (v0.355) declares `public virtual method Equals(other: Object?): bool`,
// so the argument sits at an `Object` LOCATION; §8.6 (v0.355) boxes a value
// type used there — "When a value type is used where `Object` … is expected,
// the value is boxed".
//
// The `?.` spelling's FFI-buffer arm (`nco.ffi.eq`, codegen's
// `lowerNullCondCall`) lowered the argument with a bare `lowerExpr`, on the
// premise that "a CString/WString/RawBytes argument is already
// reference-shaped (a bare pointer), so no §8.6 boxing is needed". The
// premise is about the ARGUMENT's SHAPE, but the argument is NOT restricted
// to an FFI buffer: it may be ANY value type, and §8.6 then REQUIRES it to
// be boxed. So `c?.Equals(5)` produced `icmp eq ptr %1, i32 5` — mismatched
// operand types — and the compiler ABORTED with an IR VERIFICATION FAILURE
// ("Both operands to ICmp instruction are not of the same type!").
//
// The fix lowers the argument through the SHARED §8.6 helper the other §8.1
// faces use (`lowerUniversalEqualsArg`) and DISCHARGES the fresh box's
// §28.9 `+1` after the comparison — this arm compares two POINTERS and stores
// neither, so the box cannot escape.
//
// ── #8348 ── the `?.` spelling of a COLLECTION MEMBER bypasses every
//    per-family call-surface gate (§7.5 / stdlib §1 / §4.4-§4.11 /
//    §4.17-§4.19, v0.355).
//
// The `.` spellings of the same members ARE refused by sema with the stdlib's
// own documented-surface diagnostic; the `?.` spelling was NOT — the `?.`
// CALL is typed by a synthetic `.` TWIN whose diagnostics are DISCARDED (the
// twin is speculative), so the `.` spelling's refusal was rolled back with
// the rest of the twin's report. §7.5 (v0.350) says "`x?.M` resolves `M` as
// `x.M` does", so the two spellings of ONE member may not disagree.
//
// THE ORACLE. For #8348 the issue's symptom is a program that COMPILES, so a
// row that only checked the program's OUTPUT would pass on the base too.
// Every reject row therefore pins the compile REJECTION and its diagnostic
// text, and every control row reads the value the member returns, so a fix
// that broke a DECLARED member could not pass.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\n" + decls +
           "class P\n"
           "  public static method Main(): int\n" + body +
           "\n    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A reject row: the program must NOT compile, and the diagnostic must name
// the member and the head.
void expectRejected(const std::string& body, const char* needle) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_FALSE(r.ran);
    // fix #3895 (issue #3892): a genuine compile-time rejection, not a crash
    // or a signal — `!ran` alone cannot tell them apart.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// =======================================================================
// #8334 — the `?.` spelling of `Equals` on an FFI-buffer receiver
// =======================================================================

// The issue's own repro: `c?.Equals(5)` on a `CString?`. Pre-fix this FAILED
// TO COMPILE with an IR-verification abort:
//
//   Both operands to ICmp instruction are not of the same type!
//     %nco.ffi.eq = icmp eq ptr %1, i32 5
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// §8.3's class row is the default for these receivers (they declare no
// `Equals` override), so the comparison is an IDENTITY comparison and the
// boxed argument is never equal to the receiver: the answer is `False`.
BEZEL_TEST(nullcond_ffi_equals_8334, cstring_nullcond_equals_value_type_compiles) {
    // per spec §8.1 / §8.6 / §8.3 (v0.355)
    expectStdout("    using var c: CString? := CString.From(\"hi\");\n"
                 "    Console.WriteLine(c?.Equals(5) ?? false);",
                 "False\n");
}

// The `WString` twin — the same FFI-buffer trio, the same defect.
BEZEL_TEST(nullcond_ffi_equals_8334, wstring_nullcond_equals_value_type_compiles) {
    // per spec §8.1 / §8.6 / §8.3 (v0.355)
    expectStdout("    using var w: WString? := WString.From(\"hi\");\n"
                 "    Console.WriteLine(w?.Equals(5) ?? false);",
                 "False\n");
}

// The `RawBytes` twin — the same FFI-buffer trio, the same defect.
BEZEL_TEST(nullcond_ffi_equals_8334, rawbytes_nullcond_equals_value_type_compiles) {
    // per spec §8.1 / §8.6 / §8.3 (v0.355)
    expectStdout("    using var r: RawBytes? := RawBytes.From([1, 2, 3]);\n"
                 "    Console.WriteLine(r?.Equals(5) ?? false);",
                 "False\n");
}

// CONTROL — the `.` spelling of the same call is unchanged: it already
// compiled and printed `False` (the `.` spelling's §8.6 boxing is in the
// chain), and this fix must not change it.
BEZEL_TEST(nullcond_ffi_equals_8334, cstring_dot_equals_value_type_still_works) {
    // per spec §8.1 / §8.6 / §8.3 (v0.355)
    expectStdout("    using var c: CString := CString.From(\"hi\");\n"
                 "    Console.WriteLine(c.Equals(5));",
                 "False\n");
}

// CONTROL — a REFERENCE-typed argument needs no §8.6 boxing, so no box is
// minted and no release is emitted; the answer is unchanged.
BEZEL_TEST(nullcond_ffi_equals_8334, cstring_nullcond_equals_reference_arg_still_works) {
    // per spec §8.1 / §8.6 / §8.3 (v0.355)
    expectStdout("    using var c: CString? := CString.From(\"hi\");\n"
                 "    var o: Object := 5;\n"
                 "    Console.WriteLine(c?.Equals(o) ?? false);",
                 "False\n");
}

// CONTROL — the `?.` spelling of `Equals` on an ordinary CLASS receiver is
// unchanged: it dispatches through the vtable and is not this arm.
BEZEL_TEST(nullcond_ffi_equals_8334, user_class_nullcond_equals_still_works) {
    // per spec §8.1 / §8.3 (v0.355)
    const char* decls =
        "class Box\n"
        "  public V: int;\n"
        "  public constructor(v: int)\n"
        "    V := v;\n"
        "  end constructor;\n"
        "end Box;\n";
    auto r = runAot(prog(decls,
                         "    var b: Box? := new Box(21);\n"
                         "    Console.WriteLine(b?.Equals(5) ?? false);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// =======================================================================
// #8348 — the `?.` spelling of a collection member goes through the SAME
//          per-head surface gate the `.` spelling goes through
// =======================================================================

// stdlib §4.10 (v0.355): `Set<T>` declares no `IndexOf`. The `.` spelling
// is refused with the stdlib's documented-surface diagnostic; the `?.`
// spelling was ACCEPTED (and then SILENTLY DEGRADED — codegen's
// `lowerNullCondCall` never reaches the per-family call-surface gate, so
// the call lowered to nothing behind the #3079 warning).
BEZEL_TEST(nullcond_collection_surface_8348, set_nullcond_indexof_is_rejected) {
    // per spec §7.5 (v0.350) and stdlib §4.10 (v0.355)
    expectRejected("    var s: Set<int>? := null;\n"
                   "    Console.WriteLine(s?.IndexOf(1) ?? -7);",
                   "no member `IndexOf` on `Set`");
}

// stdlib §4.8 (v0.355): `Dictionary<K, V>` declares no `IndexOf` either.
BEZEL_TEST(nullcond_collection_surface_8348, dictionary_nullcond_indexof_is_rejected) {
    // per spec §7.5 (v0.350) and stdlib §4.8 (v0.355)
    expectRejected("    var d: Dictionary<int, int>? := null;\n"
                   "    Console.WriteLine(d?.IndexOf(1) ?? -7);",
                   "no member `IndexOf` on `Dictionary`");
}

// stdlib §4.19 (v0.355): `FrozenArray<T>` declares `Length`, the indexer,
// `ToArray` and the static `From` — no `IndexOf`.
BEZEL_TEST(nullcond_collection_surface_8348, frozen_array_nullcond_indexof_is_rejected) {
    // per spec §7.5 (v0.350) and stdlib §4.19 (v0.355)
    expectRejected("    var a: Array<int> := [1, 2, 1];\n"
                   "    using var f := FrozenArray.From(a);\n"
                   "    var g: FrozenArray<int>? := f;\n"
                   "    Console.WriteLine(g?.IndexOf(1) ?? -7);",
                   "no member `IndexOf` on `FrozenArray`");
}

// stdlib §4.17 (v0.355): `SortedDictionary<K, V>` declares no `IndexOf`.
// The `.` spelling is refused with the same sentence — the `?.` spelling
// must give the SAME diagnostic, and must NOT report it twice.
BEZEL_TEST(nullcond_collection_surface_8348, sorted_dictionary_nullcond_indexof_is_rejected) {
    // per spec §7.5 (v0.350) and stdlib §4.17 (v0.355)
    expectRejected("    var d: SortedDictionary<int, int>? := null;\n"
                   "    Console.WriteLine(d?.IndexOf(1) ?? -7);",
                   "no member `IndexOf` on `SortedDictionary`");
}

// The diagnostic is the stdlib's own documented-surface sentence — the SAME
// one the `.` spelling emits (same table, same stdlib section, same quoted
// surface), so the two spellings of one member cannot disagree about it.
BEZEL_TEST(nullcond_collection_surface_8348,
           nullcond_reject_names_the_stdlib_section_and_surface) {
    // per spec §7.5 (v0.350) and stdlib §4.10 (v0.355)
    auto r = runAot(prog("", "    var s: Set<int>? := null;\n"
                              "    Console.WriteLine(s?.IndexOf(1) ?? -7);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(stdlib \xC2\xA7""4.10)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not part of this collection's documented surface"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "language spec \xC2\xA7""8.1"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "stdlib \xC2\xA7""5 LINQ extension operator"));
}

// CONTROL — stdlib §4.4 (v0.355): `Array<T>` DOES declare `IndexOf(item: T,
// startIndex: int := 0): int`. The `?.` spelling must still be ADMITTED, and
// must lower to the SAME value the `.` spelling lowers to — not a silently
// degraded zero.
BEZEL_TEST(nullcond_collection_surface_8348, array_nullcond_indexof_is_admitted) {
    // per spec §7.5 (v0.350) and stdlib §4.4 (v0.355)
    expectStdout("    var a: Array<int>? := [1, 2, 1];\n"
                 "    Console.WriteLine(a?.IndexOf(2) ?? -7);",
                 "1\n");
}

// The same CONTROL over a NULL receiver: the `?.` spelling yields null, so
// the `??` fallback answers. The `?.` spelling is ADMITTED here.
BEZEL_TEST(nullcond_collection_surface_8348, array_nullcond_indexof_on_null_receiver) {
    // per spec §7.5 (v0.350) and stdlib §4.4 (v0.355)
    expectStdout("    var a: Array<int>? := null;\n"
                 "    Console.WriteLine(a?.IndexOf(1) ?? -7);",
                 "-7\n");
}

// CONTROL — a DECLARED member of another head lowers to the same value
// through the `?.` spelling as through the `.` spelling (stdlib §4.10's
// `Set<T>.Count`, stdlib §4.19's `FrozenArray<T>.Length`).
BEZEL_TEST(nullcond_collection_surface_8348, declared_members_lower_to_the_same_value) {
    // per spec §7.5 (v0.350), stdlib §4.10 / §4.19 (v0.355)
    expectStdout("    var s: Set<int>? := new Set<int>();\n"
                 "    Console.WriteLine(s?.Count ?? -7);\n"
                 "    var a: Array<int> := [1, 2, 1];\n"
                 "    using var f := FrozenArray.From(a);\n"
                 "    var g: FrozenArray<int>? := f;\n"
                 "    Console.WriteLine(g?.Length ?? -7);",
                 "0\n3\n");
}

// CONTROL — §26.5 rule 2: a USER extension that declares the name on the
// receiver IS the member (stdlib §1's Block-by-name rule blocks an
// extension only by the NAME the head declares), so the `?.` spelling of it
// is admitted and dispatches to the extension.
BEZEL_TEST(nullcond_collection_surface_8348,
           user_extension_still_reaches_the_nullcond_spelling) {
    // per spec §26.5 (v0.350) and stdlib §1's Block-by-name rule
    const char* decls =
        "extension Set<int>\n"
        "  method IndexOf(item: int): int\n"
        "    return 42;\n"
        "  end IndexOf;\n"
        "end extension;\n";
    auto r = runAot(prog(decls,
                         "    var s: Set<int>? := new Set<int>();\n"
                         "    Console.WriteLine(s?.IndexOf(1) ?? -7);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}
