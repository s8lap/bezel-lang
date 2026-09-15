// fix #4114 — a choice implementing a PLAIN (non-special) user interface with
// NO explicit ToString/Equals/GetHashCode override SEGFAULTED bezelc at
// codegen time while building the choice's box default-ToString thunk
// (spec §35.1/§35.8, v0.295).
//
// ── ROOT CAUSE (measured with gdb + temporary instrumentation, not guessed)
// `setupBoxDescriptors`'s Pass C (LlvmBackend_Setup.cpp, ~line 35309) builds
// each boxable type's interface itable. Before this fix, its "ensure the
// struct layout is populated before ensurePropertyAccessorFn" step called
// `classStruct(ts)` UNCONDITIONALLY for every `ts` reaching that point —
// including a `Choice`, which fix #3900 had newly added to this same Pass
// A-D machinery ("Pass A-D below are already kind-agnostic"). But
// `classStruct` (LlvmBackend_Layout.cpp) had a routing arm for
// `TypeDeclKind::Record` only:
//     if (t->kind == ast::TypeDeclKind::Record) return recordStruct(t);
// A Choice fell through to classStruct's GENERIC path, which built a
// class-shaped `{ vtable*, i8 disposed-flag }` struct (a choice has NO
// instance fields — §35.1: "Choices cannot have ... instance fields") and
// CACHED it into the SHARED `structs_` map, keyed on `t` — the exact map
// `choiceStruct(t)` itself reads via the SAME key. Later in the SAME `ts`
// iteration, `boxToStringThunk(ts)`'s choice branch calls `choiceStruct(ts)`
// to synthesize the §35.7 default `ToString` (no user override present) and
// retrieves the ALREADY-POISONED `{ptr, i8}` entry instead of the real
// `{i32 tag, case0Payload, case1Payload, …}` layout. `emitChoiceToString`
// (LlvmBackend_String.cpp:3756) then does a two-level
// `CreateExtractValue(val, {1+caseIndex, fieldIndex})` against that
// two-field, non-nested struct: the second index cannot descend further
// into the `i8` field, LLVM's (non-assertions) release build resolves this
// to a null-typed extract, and constructing that Value crashes inside
// `llvm::Value::setName` — a SIGSEGV, not a bounds check this file could
// catch on its own.
//
// ── THE FIX ─────────────────────────────────────────────────────────────
// `classStruct` (LlvmBackend_Layout.cpp) gained a second routing arm,
// mirroring the existing Record one exactly:
//     if (t->kind == ast::TypeDeclKind::Choice) return choiceStruct(t);
// This makes it impossible for ANY caller (Pass C above, or any future one)
// to build/cache a bogus class-shaped struct for a Choice TypeSymbol — the
// SAME structural guarantee Record already had. `choiceStruct` self-manages
// its own generic-instantiation caching, exactly like `recordStruct`, so
// delegating at the top of `classStruct` (before its own generic-subst
// logic) is the same pattern already proven for Record.
//
// ── NARROWING GRID (measured on origin/main 102d29f, pre-fix vs. this
// fix; see the fix commit body for the "N of M" FAIL-WITHOUT tally) ────────
//   - non-generic choice, arrow-body method, no override trio:  CRASH → OK
//   - non-generic choice, `case this of` method body, no trio:  CRASH → OK
//   - generic choice, `case this of` method body, no trio:      CRASH → OK
//   - choice implementing a GENERIC interface, no trio:         CRASH → OK
//   - choice implementing TWO plain interfaces, no trio:        CRASH → OK
//   - generic choice, arrow-body method, no trio:      no crash either side
//   - non-generic choice WITH the override trio declared:
//                                                       no crash either side
//   - record implementing the same plain interface:    no crash either side
//   - class implementing the same plain interface:     no crash either side
// The crash is Choice-specific: it never reached Record or Class, because
// classStruct's pre-existing Record arm already protected records, and a
// class IS the shape classStruct's generic path correctly builds.
//
// ── WHAT EACH ROW IS FOR ────────────────────────────────────────────────
//   T1  the issue's own minimal repro (§35.1/§35.8): non-generic choice,
//       trivial arrow-body method, plain interface, no override trio.
//       Pre-fix: bezelc SIGSEGVs (exit 139) before ever reaching sema
//       diagnostics or a real IR-verification error — a bare
//       `EXPECT_FALSE(r.ran)` would NOT distinguish this from a legitimate
//       rejection (lane rule: pair with message-text, not just failure).
//   T2  the same shape with a `case this of` method body (narrower —
//       exercises a second, independently-reached codegen path per the
//       issue's own narrowing notes). Distinct from #4113, which crashes an
//       expression-bodied PROPERTY whose body is `case this of` — this row
//       uses an ordinary METHOD body, unaffected by #4113.
//   T3  forces an actual box conversion (assigning to an interface-typed
//       local) and calls `.ToString()` through the box's synthesized slot —
//       the exact function that crashed (`emitChoiceToString` via
//       `boxToStringThunk`) — and pins the §35.7 CORRECT output text, not
//       merely "did not crash".
//   T4  ACCEPT-SIDE CONTROL: a GENERIC choice with a trivial method body and
//       no override trio never crashed even pre-fix (see the grid above) —
//       pins that this fix does not disturb it.
//   T5  ACCEPT-SIDE CONTROL: the issue's own documented workaround (the
//       override trio declared) keeps compiling and running correctly.
//   T6  REGRESSION GUARD: a record and a class implementing the identical
//       plain interface shape are unaffected by classStruct's new Choice
//       arm (Record already had its own arm; Class is untouched).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

// ── T1 — the issue's own minimal repro (§35.1/§35.8). Pre-fix: SIGSEGV
// (CompileOutcome::Crashed) at codegen time, before Main ever runs.
BEZEL_TEST(codegen_choice_plain_interface_boxtostring_crash_4114,
           nongeneric_choice_plain_interface_trivial_body_no_trio_compiles_and_runs_spec_35_1_35_8) {
    auto r = runAot(
        "module M4114A; "
        "interface IShape4114A method Area(): double; end IShape4114A; "
        "choice Shape4114A: IShape4114A "
        "  Circle4114A(radius: double), Square4114A(side: double); "
        "  public method Area(): double => 4.0; "
        "end Shape4114A; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4114A.Circle4114A(2.0); "
        "    return (if s.Area() = 4.0 then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T2 — same shape, `case this of` METHOD body (a distinct codegen path
// from #4113's expression-bodied PROPERTY case, per the issue's own
// narrowing notes).
BEZEL_TEST(codegen_choice_plain_interface_boxtostring_crash_4114,
           nongeneric_choice_plain_interface_case_match_method_body_no_trio_compiles_and_runs_spec_35_1_35_5_35_8) {
    auto r = runAot(
        "module M4114B; "
        "interface IShape4114B method Area(): double; end IShape4114B; "
        "choice Shape4114B: IShape4114B "
        "  Circle4114B(radius: double), Square4114B(side: double); "
        "  public method Area(): double => "
        "    (case this of "
        "       Circle4114B(r) => r * r * 3.0, "
        "       Square4114B(s) => s * s); "
        "end Shape4114B; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4114B.Square4114B(3.0); "
        "    return (if s.Area() = 9.0 then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T3 — forces the actual box conversion and calls `.ToString()` through
// the box's synthesized slot: the exact function that crashed
// (emitChoiceToString via boxToStringThunk), pinning the CORRECT §35.7
// output ("TypeName.CaseName(field: value)"), not merely "did not crash".
BEZEL_TEST(codegen_choice_plain_interface_boxtostring_crash_4114,
           boxed_choice_synthesizes_default_tostring_via_interface_reference_spec_35_7_35_8) {
    auto r = runAot(
        "module M4114C; "
        "interface IShape4114C method Area(): int; end IShape4114C; "
        "choice Shape4114C: IShape4114C "
        "  Circle4114C(radius: int), Square4114C(side: int); "
        "  public method Area(): int => 4; "
        "end Shape4114C; "
        "class C "
        "  public static method Main(): int "
        "    var i: IShape4114C := Shape4114C.Circle4114C(7); "
        "    var o: Object := i; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("Shape4114C.Circle4114C(radius: 7)\n"));
}

// ── T4 — ACCEPT-SIDE CONTROL: a GENERIC choice with a trivial method body
// and no override trio never crashed even pre-fix; pin it stays that way.
BEZEL_TEST(codegen_choice_plain_interface_boxtostring_crash_4114,
           generic_choice_plain_interface_trivial_body_no_trio_still_compiles_spec_35_1_35_8_35_11_accept_control) {
    auto r = runAot(
        "module M4114D; "
        "interface IShape4114D method Area(): double; end IShape4114D; "
        "choice Shape4114D<T>: IShape4114D "
        "  Circle4114D(radius: double), Square4114D(side: T); "
        "  public method Area(): double => 4.0; "
        "end Shape4114D; "
        "class C "
        "  public static method Main(): int "
        "    var s: Shape4114D<double> := Shape4114D<double>.Circle4114D(2.0); "
        "    return (if s.Area() = 4.0 then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T5 — ACCEPT-SIDE CONTROL: the issue's own documented workaround (the
// override trio declared) keeps compiling and running correctly.
BEZEL_TEST(codegen_choice_plain_interface_boxtostring_crash_4114,
           nongeneric_choice_plain_interface_with_override_trio_still_compiles_and_runs_spec_35_2_35_7_35_8_accept_control) {
    auto r = runAot(
        "module M4114E; "
        "interface IShape4114E method Area(): double; end IShape4114E; "
        "choice Shape4114E: IShape4114E "
        "  Circle4114E(radius: double), Square4114E(side: double); "
        "  public method Area(): double => 4.0; "
        "  public override method ToString(): string => \"Shape4114E\"; "
        "  public method Equals(other: Shape4114E): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Shape4114E; "
        "class C "
        "  public static method Main(): int "
        "    var s := Shape4114E.Circle4114E(2.0); "
        "    return (if s.Area() = 4.0 then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T6 — REGRESSION GUARD: a record and a class implementing the identical
// plain-interface shape are unaffected by classStruct's new Choice arm.
BEZEL_TEST(codegen_choice_plain_interface_boxtostring_crash_4114,
           record_and_class_plain_interface_unaffected_spec_9_35_8_accept_control) {
    auto rRecord = runAot(
        "module M4114F; "
        "interface IShape4114F method Area(): double; end IShape4114F; "
        "record RShape4114F(Radius: double): IShape4114F "
        "  public method Area(): double => 4.0; "
        "end RShape4114F; "
        "class C "
        "  public static method Main(): int "
        "    var s := new RShape4114F(2.0); "
        "    return (if s.Area() = 4.0 then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(rRecord, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(rRecord.ran);
    BEZEL_EXPECT_EQ(rRecord.exitCode, 42);

    auto rClass = runAot(
        "module M4114G; "
        "interface IShape4114G method Area(): double; end IShape4114G; "
        "class CShape4114G: IShape4114G "
        "  public method Area(): double => 4.0; "
        "end CShape4114G; "
        "class C "
        "  public static method Main(): int "
        "    var s := new CShape4114G(); "
        "    return (if s.Area() = 4.0 then 42 else 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(rClass, "compiler CRASHED"));
    BEZEL_EXPECT_TRUE(rClass.ran);
    BEZEL_EXPECT_EQ(rClass.exitCode, 42);
}
