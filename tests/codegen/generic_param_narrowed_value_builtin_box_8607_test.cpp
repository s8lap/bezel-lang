// Codegen — §7.4 "Member access on nullable values" (the narrowing) / §22.6
// ("a read of a binding §7.4 has narrowed non-null is of its non-nullable
// type") / §14 (generic calls) / §8.6 "Boxing" / stdlib §2.5-§2.6 / #8607
// (spec v0.357): a §7.4-NARROWED nullable built-in VALUE type passed to a
// generic method's `T` parameter must arrive as the PRESENT value.
//
// THE DEFECT (MEASURED on the parent, r30-c1 @ 4d689965e):
//   `var n: Guid? := Guid.Empty; var b := Bx<Guid>(n);`
//   -> `b.ToString()` printed `01000000-0000-0000-0000-000000000000`
//      where `00000000-0000-0000-0000-000000000000` is required, and the
//      `DateTime?` twin printed `0001-01-01T00:00:01` where
//      `2020-01-01T00:00:00` is required. rc=0 — a SILENT wrong value.
//
// THE CAUSE. The first byte of the box's payload was the carrier's
// `hasValue` tag: the `{i8 hasValue; T}` carrier was handed to the `T`
// parameter UN-STRIPPED, and the generic body boxed the CARRIER (so the box
// held 24 bytes where the bare `Guid` is 16), and `Guid.ToString` read the
// tag byte as the Guid's first word.
//
// The strip is the §22.6 one: "a read of a binding §7.4 has narrowed
// non-null is of its non-nullable type", published to codegen by
// `SemaContext::markNarrowedValueRead` and performed by `Emitter::lowerExpr`'s
// unwrap (the #6674 pair). Sema published the read only when the source
// type carried a DECLARATION (`from->kind == Primitive || from->declaration`)
// — but a value-builtin spelled as an ANNOTATION (`var n: Guid?`) resolves to
// a DECLARATION-LESS Named ref (the resolver keeps `declaration = nullptr`
// for the value-builtins, see `isValueBuiltinByName`'s own note). So the
// read was never published, the carrier was never stripped, and the generic
// call received it whole. The RECORD twin (`Bx<Pt>(p)`) was correct because
// a record's TypeRef DOES carry its declaration, and the PRIMITIVE twin
// (`Bx<int>(i)`) because a primitive's kind is `Primitive`.
//
// THE SPEC RULE. §7.4: a local declared `T?` and initialized with a
// non-null value is narrowed non-null, so at the call site the read is of
// its NON-NULLABLE type (§22.6) — the bare `Guid`, not the carrier. §8.6:
// "The wrapper's payload is fixed at boxing: a value enters a box by being
// copied in and leaves it by being read out" — the box holds the bare
// `Guid`, whose bytes are the Guid's.
//
// ORACLE: exact stdout. The `Guid` row runs on the CHECKED (ASan/LSan) leg;
// the `DateTime` / `TimeSpan` rows run UNCHECKED because the box that arrives
// from a CALL is not always released — a SEPARATE, PRE-EXISTING §28.9 defect
// that reproduces with a NON-narrowed source and with a NON-generic callee
// too (so it is not this fix's doing) and is filed on its own as issue
// #8629. The observable those rows claim is the VALUE the issue reports.
//
// The `DateTime` BOUND spelling (`var b := Bx<DateTime>(d);`) leaks the same
// way — that is issue #8632.
//
// BOUNDARY CONTROL (this suite's ONE base-green row). The §7.4 narrowing is
// what admits the argument: an UN-narrowed `Guid?` (a local whose value may
// be null) is not assignable to the non-nullable `Guid` the `T` parameter
// is bound to, so it stays a compile error (§7). That rejection is the sema
// gate the fix's unwrap rests on, and this row catches THE WRONG-LAYER FIX:
// a fix made in SEMA — one that admits `Guid?` → `Guid` at the call site by
// dropping the §7.4 narrowing gate instead of publishing the narrowed read —
// would make this program compile, and the row would go RED.
//
// The other base-green controls of this family (the `n!` spelling, the
// non-nullable `n: Guid` twin, the `Id<T>(x: T): T` twin that does not box,
// and the non-generic `F(g: Guid): Object` twin) are green with AND without
// the fix and their verdicts are fixed by a spec sentence, so they are
// CORPUS CANDIDATES, not gtest rows — they live at
//   /var/tmp/bezel-scratch/r42-m36-8593/corpus-candidates/.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro, verbatim (#8607).
std::string guidGenericBox() {
    return "module M; "
           "class C "
           "  static method Bx<T>(x: T): Object "
           "    return x; "
           "  end Bx; "
           "  public static method Main(): int "
           "    var n: Guid? := Guid.Empty; "
           "    var b := Bx<Guid>(n); "
           "    Console.WriteLine(b.ToString()); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `DateTime` twin of the issue's repro (stdlib §2.5), VERBATIM: the
// result is printed through an INLINE receiver. The row runs UNCHECKED
// because that spelling trips a SEPARATE, PRE-EXISTING §28.9 defect — a box
// that arrives from a CALL is not always released (it reproduces with a
// NON-narrowed source and with a NON-generic callee too, so it is not this
// fix's doing; filed on its own). The observable this row claims is the
// VALUE the issue reports.
std::string dateTimeGenericBox() {
    return "module M; "
           "class C "
           "  static method Bx<T>(x: T): Object "
           "    return x; "
           "  end Bx; "
           "  public static method Main(): int "
           "    var d: DateTime? := new DateTime(2020,1,1); "
           "    Console.WriteLine(Bx<DateTime>(d).ToString()); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `TimeSpan` twin (stdlib §2.5).
std::string timeSpanGenericBox() {
    return "module M; "
           "class C "
           "  static method Bx<T>(x: T): Object "
           "    return x; "
           "  end Bx; "
           "  public static method Main(): int "
           "    var t: TimeSpan? := new TimeSpan(1,2,3); "
           "    Console.WriteLine(Bx<TimeSpan>(t).ToString()); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// THE BOUNDARY CONTROL. An UN-narrowed `Guid?` (no §7.4 fact) is not
// assignable to the non-nullable `Guid` the `T` parameter binds to, so it
// stays a compile error (§7).
std::string unnarrowedGuidAtGenericFormal() {
    return "module M; "
           "class C "
           "  static method Bx<T>(x: T): Object "
           "    return x; "
           "  end Bx; "
           "  public static method Main(): int "
           "    var n: Guid? := null; "
           "    var b := Bx<Guid>(n); "
           "    Console.WriteLine(b.ToString()); "
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

// The issue's own repro: a §7.4-narrowed `Guid?` (stdlib §2.6) into a
// generic `T` parameter the body boxes to `Object` (§8.6).
BEZEL_TEST(codegen_generic_param_narrowed_value_builtin_box_8607,
           guid_narrowed_into_a_generic_t_parameter_boxes_the_present_value) {
    expectStdout(guidGenericBox(),
                 "00000000-0000-0000-0000-000000000000\n");
}

// The `DateTime` twin of the issue's repro (stdlib §2.5), VERBATIM. UNCHECKED
// for the reason named on the helper above (the separate §28.9 release
// defect); the observable claimed here is the VALUE.
BEZEL_TEST(codegen_generic_param_narrowed_value_builtin_box_8607,
           datetime_narrowed_into_a_generic_t_parameter_boxes_the_present_value) {
    JitResult r = runAot(dateTimeGenericBox());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2020-01-01T00:00:00\n");
}

// The `TimeSpan` twin (stdlib §2.5). UNCHECKED for the same reason as the
// `DateTime` row above (the separate §28.9 release defect); the observable
// claimed here is the VALUE.
BEZEL_TEST(codegen_generic_param_narrowed_value_builtin_box_8607,
           timespan_narrowed_into_a_generic_t_parameter_boxes_the_present_value) {
    JitResult r = runAot(timeSpanGenericBox());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "01:02:03\n");
}

// THE BOUNDARY CONTROL (the suite's one base-green row). §7's nullable
// rule: an un-narrowed `Guid?` is not assignable to the non-nullable
// `Guid` the `T` parameter binds to, so it stays a compile error. This row
// catches THE WRONG-LAYER FIX: a SEMA fix that admits `Guid?` → `Guid` at
// the call site by dropping the §7.4 narrowing gate (instead of publishing
// the narrowed read) would make this program compile, and the row would go
// RED.
BEZEL_TEST(codegen_generic_param_narrowed_value_builtin_box_8607,
           unnarrowed_value_builtin_at_a_nonnullable_generic_formal_is_rejected) {
    JitResult r = runAot(unnarrowedGuidAtGenericFormal());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "nullable"));
}
