// Codegen — §8.6 "Nullable value-type boxing" / §5.1 "Reference conversions" /
// §7.4 "Member access on nullable values" / stdlib §2.5-§2.6 / #8476
// (spec v0.357): a §7.4-NARROWED nullable value type converted to an
// `Object`/interface slot must be BOXED — the §8.6 box of the PRESENT value.
//
// THE DEFECT (MEASURED on the parent, r30-c1 @ 0cb32bb14):
//   `var n: Guid? := Guid.Empty; var g: IComparable<Guid> := n;`
//   -> the program compiled, then `g.CompareTo(Guid.Empty)` failed to lower
//      ("internal: the argument of this `Console.WriteLine` failed to lower to
//      a value, so the whole statement emits nothing" — #3079), so the program
//      PRINTED NOTHING and exited 0.
//   -> the same program with `DateTime?` SEGV'd (rc=139), and the Guid twin
//      sometimes aborted with "refcount over-release (box) — two releases for
//      one reference" (rc=134) instead: the failure was NONDETERMINISTIC.
//
// THE CAUSE: the source's sema type is the DECLARED `Guid?` (sema records the
// §7.4 narrowing in `checkAssignable`, not in the expression's type), and the
// §8.6 box helper looked the box VTABLE up for that NULLABLE type. A nullable
// VALUE-BUILTIN has no vtable of its own — `valueBuiltinBoxName` excludes a
// nullable source — so NO box was minted: the `{i8 hasValue; Guid}` CARRIER
// was stored where a box POINTER belongs. Three defects followed from that one
// fact: the local's slot was sized from the carrier rather than from the
// declared pointer type (so every interface member call on it failed to
// lower -> nothing printed); the §8.6 scope-end `bz_box_release` released the
// carrier's first word as a pointer (a garbage pointer -> SIGSEGV /
// over-release); and at a call argument the carrier was passed where the
// formal's `ptr` parameter type was expected (an IR verification failure).
//
// THE SPEC RULE: §8.6 "Nullable value-type boxing" — "When a nullable value
// type `T?` is boxed: ... If the value is non-null, the result is a boxed `T`
// (the non-nullable form). The nullable wrapper is stripped during boxing."
// The box holds the BARE `T`, so it carries T's vtable.
//
// ORACLE: exact stdout under the CHECKED (ASan/LSan) leg — the observable is a
// §8.6 box, so the release is part of the claim (a leaked box, or a second
// release of one, is a failure, not a cosmetic difference).
//
// BOUNDARY CONTROL (the ONE base-green row). The owner's 2026-11-12 rule
// caps a suite at TWO base-green gtest rows and asks that each kept one be
// named with the wrong fix it catches. This suite keeps TWO base-green rows:
//
//   * `nonnullable_guid_to_icomparable_control_stdlib_2_6` catches a fix that
//     changes the NON-nullable box path: a fix that rewrites `boxValue`'s
//     ordinary `Guid` box (or its vtable choice) while adding the nullable
//     fallback would break a box that already worked, and this row goes RED.
//   * `unnarrowed_guid_to_icomparable_is_rejected_spec_5_1` catches THE
//     WRONG-LAYER FIX: a fix that admits `T?` -> `IComparable<T>` in SEMA
//     (dropping §5.1's nullable lifting) instead of fixing the §8.6 box would
//     make the un-narrowed program compile, and this row goes RED. It is the
//     soundness precondition the fix's unwrap rests on.
//
// The `int?` twin (`narrowed_int_to_icomparable_still_boxes_spec_5_1`) is a
// primitive whose nullable form already had a box vtable, so it boxed
// correctly before the fix; its verdict is fixed by §5.1 and it lives as a
// CORPUS CANDIDATE, not a gtest row, at
//   /var/tmp/bezel-scratch/r42-m29-8476/corpus-candidates/
//       narrowed_int_to_icomparable_still_boxes_spec_5_1.bzl

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAot;

namespace {

// The issue's own repro, verbatim (guid_nullable_ok.bzl / #8476).
std::string guidRepro() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var n: Guid? := Guid.Empty; "
           "    var g: IComparable<Guid> := n; "
           "    Console.WriteLine(g.CompareTo(Guid.Empty)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The issue's own repro, verbatim (dt_nullable_ok.bzl / #8476).
std::string dateTimeRepro() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var n: DateTime? := new DateTime(2020,1,1); "
           "    var g: IComparable<DateTime> := n; "
           "    Console.WriteLine(g.CompareTo(new DateTime(2020,1,1))); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `Object` slot (§5.1's "any type to `Object`", by boxing) over the same
// narrowed nullable value.
std::string guidToObject() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var n: Guid? := Guid.Empty; "
           "    var o: Object := n; "
           "    Console.WriteLine(o.ToString()); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The CALL-ARGUMENT position: the narrowed nullable reaching an
// `IComparable<Guid>` formal. On the parent this was an IR verification
// failure ("Call parameter type does not match function signature!") — the
// carrier was passed where the formal's `ptr` parameter type was expected.
std::string guidCallArgument() {
    return "module M; "
           "class C "
           "  static method Take(g: IComparable<Guid>): int "
           "    return g.CompareTo(Guid.Empty); "
           "  end Take; "
           "  public static method Main(): int "
           "    var n: Guid? := Guid.Empty; "
           "    Console.WriteLine(Take(n)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// §5.1's nullable lift: `T?` converts at most to `Object?`, never to
// `Object`. An UN-narrowed `Guid?` must therefore still be REJECTED at a
// non-nullable interface slot — the sema gate the fix's unwrap rests on.
std::string guidNotNarrowed() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var n: Guid? := null; "
           "    var g: IComparable<Guid> := n; "
           "    Console.WriteLine(g.CompareTo(Guid.Empty)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The NON-nullable twin of the issue's Guid repro (stdlib §2.6): the source is
// a plain `Guid`, so there is no narrowing and no nullable carrier. It already
// boxed correctly before the fix; it is the control for a fix that disturbs
// the ordinary box path.
std::string guidNonNullableRepro() {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    var n: Guid := Guid.Empty; "
           "    var g: IComparable<Guid> := n; "
           "    Console.WriteLine(g.CompareTo(Guid.Empty)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectStdout(const std::string& src, const char* expected) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The issue's own repro, verbatim: stdlib §2.6 (`Guid` declares
// `IComparable<Guid>, IEquatable<Guid>`), narrowed by §7.4, boxed by §8.6.
BEZEL_TEST(codegen_boxed_value_builtin_nullable_narrowed_8476,
           guid_through_icomparable_after_narrowing_stdlib_2_6) {
    expectStdout(guidRepro(), "0\n");
}

// The issue's own repro, verbatim: stdlib §2.5 (`DateTime` declares
// `IComparable<DateTime>, IEquatable<DateTime>`).
BEZEL_TEST(codegen_boxed_value_builtin_nullable_narrowed_8476,
           datetime_through_icomparable_after_narrowing_stdlib_2_5) {
    expectStdout(dateTimeRepro(), "0\n");
}

// The §5.1 `Object` slot over the same narrowed nullable value.
BEZEL_TEST(codegen_boxed_value_builtin_nullable_narrowed_8476,
           narrowed_guid_to_object_slot_spec_5_1) {
    expectStdout(guidToObject(), "00000000-0000-0000-0000-000000000000\n");
}

// The call-argument position (the parent's IR verification failure).
BEZEL_TEST(codegen_boxed_value_builtin_nullable_narrowed_8476,
           narrowed_guid_to_icomparable_parameter_spec_5_1) {
    expectStdout(guidCallArgument(), "0\n");
}

// BOUNDARY CONTROL 1 of 2 (base-green by design). The non-nullable `Guid`
// box already worked on base. It catches a fix that disturbs the ordinary box
// path (a rewritten `boxValue` vtable choice that also changes a non-nullable
// box) — that fix would turn this row RED.
BEZEL_TEST(codegen_boxed_value_builtin_nullable_narrowed_8476,
           nonnullable_guid_to_icomparable_control_stdlib_2_6) {
    expectStdout(guidNonNullableRepro(), "0\n");
}

// BOUNDARY CONTROL 2 of 2 (base-green by design). §5.1's nullable
// lift admits `T?` at most at `T?`, so an un-narrowed `Guid?` stays a
// compile error at a non-nullable interface slot. This is the sema gate the
// fix's unwrap depends on — it catches THE WRONG-LAYER FIX: a fix that
// admits `T?` → `IComparable<T>` in SEMA instead of fixing the §8.6 box
// would make this program compile, and the row would go RED.
BEZEL_TEST(codegen_boxed_value_builtin_nullable_narrowed_8476,
           unnarrowed_guid_to_icomparable_is_rejected_spec_5_1) {
    JitResult r = runAot(guidNotNarrowed());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "nullable"));
}
