// Regression tests for #8004 — a member call on a §12.7-narrowed `Object`
// carrier at a builtin the compiler CANNOT narrow to a dispatchable builtin.
//
// THE RULE. Spec §12.7 (v0.350) narrows a §7.4 narrowing carrier to the
// tested type inside the controlled block, and §8.1 (v0.350) fixes the root
// `Object`'s public surface as EXACTLY five members:
//
//   public class Object
//     public method GetType(): Type;
//     public virtual method Equals(other: Object?): bool;
//     public virtual method GetHashCode(): int;
//     public virtual method ToString(): string;
//     public static method ReferenceEquals(a: Object?, b: Object?): bool;
//   end Object;
//
// §26.5 rule 1 commits the instance lookup against the receiver's DECLARED
// type.  So a member the carrier's declared `Object` does not declare is a
// COMPILE ERROR — and the closed surface's own refusal says so.
//
// THE DEFECT (#8004).  The §12.7 narrowing reaches the heap-pointer builtins
// (`common::isHeapPointerReferenceBuiltin`), so `if o is StringBuilder then
// o.Append("hi")` dispatches.  But at the builtins the narrowing does NOT
// reach — `TextWriter`, `TextReader`, `Encoding`, `Stream`, and every
// GENERIC heap builtin (`IEnumerable<T>`, `IReadOnlyList<T>`, `Array<T>`, …) —
// the carrier keeps its declared `Object` type inside the block, and the
// member call was ACCEPTED AND NOT DISPATCHED: the program compiled, the call
// emitted nothing, and the only trace was an `internal:` warning
// (#2477 / #2547 / #3079) — a silent wrong answer.
//
// THE FIX (the issue's own second option).  "A member call on a carrier the
// compiler cannot narrow to a dispatchable builtin is REFUSED with a
// diagnostic, instead of being accepted and dropped."  MEASURED at this tip:
// every affected head is refused with §8.1's closed-surface diagnostic.
//
// EVERY ROW HERE IS A POSITIVE OBSERVABLE: each asserts the EXACT diagnostic
// text of the refusal.  Nothing asserts the absence of a diagnostic, so no
// row is vacuous.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §8.1's closed-surface refusal, as the closed surface words it.
std::string closedSurfaceRefusal(const std::string& member) {
    return "no member `" + member + "` on type `Object`: `Object`'s public "
           "surface is exactly `GetType`, `Equals`, `GetHashCode`, `ToString` "
           "and the static `ReferenceEquals` (\xC2\xA7""8.1); members resolve "
           "against the declared type (\xC2\xA7""26.5), and no extension on "
           "`Object` provides `" +
           member + "`";
}

// The §12.7 shape: an `Object` carrier, an `is` test against a builtin the
// narrowing does not reach, and a member read of the carrier inside the
// controlled block.
std::string program(const std::string& instance, const std::string& tested,
                    const std::string& member) {
    return "module M; public class P public static method Main(): int "
           "  var o: Object := " +
           instance + "; "
           "  if o is " +
           tested + " then "
           "    Console.WriteLine(\"in\"); "
           "    _ := o." +
           member + "; "
           "    Console.WriteLine(\"after\"); "
           "  end; "
           "  return 0; end Main; end P;";
}

}  // namespace

// ── The NON-GENERIC heap builtins the narrowing does not reach ──────────────

BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           textwriter_member_call_is_refused_spec_12_7) {
    auto r = runAot(program("Console.Out", "TextWriter", "Write"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, closedSurfaceRefusal("Write").c_str()));
}

BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           textreader_member_call_is_refused_spec_12_7) {
    auto r = runAot(program("Console.In", "TextReader", "ReadToEnd"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, closedSurfaceRefusal("ReadToEnd").c_str()));
}

BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           encoding_member_call_is_refused_spec_12_7) {
    auto r = runAot(program("Encoding.UTF8", "Encoding", "GetString"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, closedSurfaceRefusal("GetString").c_str()));
}

// `Stream` (stdlib §7.1) — the fifth head the issue names.  The program never
// runs (the member call is refused at compile time), so the `FileStream` the
// carrier holds is never opened.
BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           stream_member_call_is_refused_spec_12_7) {
    auto r = runAot(program("new FileStream(\"/tmp/bezel8004.bin\", FileMode.Create)",
                            "Stream", "Length"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, closedSurfaceRefusal("Length").c_str()));
}

// ── The GENERIC heap builtins — a generic instantiation is excluded from the
// narrowing population, so the carrier keeps its declared `Object` type ──────

BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           array_member_call_is_refused_spec_12_7) {
    auto r = runAot(program("new Array<int>()", "Array<int>", "Add"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, closedSurfaceRefusal("Add").c_str()));
}

BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           ienumerable_member_call_is_refused_spec_12_7) {
    auto r = runAot(program("new Array<int>()", "IEnumerable<int>", "Count"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, closedSurfaceRefusal("Count").c_str()));
}

// ── THE CONTROL ROW: the heap-pointer builtin the narrowing DOES reach keeps
// its member (§12.7), so the refusal above is the closed surface's and not a
// blanket one. ───────────────────────────────────────────────────────────────
BEZEL_TEST(sema_builtin_narrow_carrier_8004,
           narrowable_heap_builtin_member_is_still_dispatched_spec_12_7) {
    auto r = runAot(
        "module M; public class P public static method Main(): int "
        "  var o: Object := new StringBuilder(); "
        "  if o is StringBuilder then "
        "    _ := o.Append(\"hi\"); "
        "    Console.WriteLine(o.Length); "
        "  end; "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}
