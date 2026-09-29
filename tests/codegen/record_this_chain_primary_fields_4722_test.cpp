// fix #4722 half (a) (§9.1 / §9.3 / §12.8 / §22.5 / §22.8 step 0, v0.307): a
// combined-form record's body constructor chaining with `this(...)` to the
// record's auto-generated primary constructor must store the chain's values
// into the positional fields.
//
// WHAT THE DEFECT WAS. `Emitter::emitChainCall` (LlvmBackend_Setup.cpp) takes
// its primary-constructor arm when no explicit constructor binds the chain
// call. That arm called `storePrimaryCtorPositionalArgs`, which returned
// early for any type that was not a CLASS, and indexed the object with
// `classStruct` rather than a record's `recordStruct` layout. Sema did not
// publish the §22.5 binding of a record chain either
// (`publishPrimaryCtorChainBinding` was class-only). So the chain stored
// nothing: the record kept its zero fill, and the program compiled clean and
// printed `R(X: 0, Y: 0)` — silent wrong code. With a `string` positional
// field it crashed.
//
// SPEC. §9.1: a positional record gets "A primary constructor whose
// parameters are the positional fields, in declaration order". §9.3:
// "Chaining with `this(...)` to the primary remains available and is governed
// by §12.8 unchanged". §22.5: named arguments bind by name.
//
// Every row asserts the printed VALUES. Half (b) of #4722 (`ctorForArgs`
// binding named arguments by name) landed as b83afbe3; the named rows here
// exercise the chain-to-primary path, which half (b) does not reach.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// The issue's p05: positional chain from a one-parameter body constructor.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           positional_chain_writes_fields_spec_9_3) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(s: string) this(s.Length, 2); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(\"abc\").ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "R(X: 3, Y: 2)\n");
}

// The issue's p04: the named form, written out of declaration order — the
// store must follow sema's §22.5 binding, not textual order.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           named_chain_binds_by_name_spec_22_5) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(s: string) this(Y := 2, X := s.Length); "
        "  end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(\"abc\").ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "R(X: 3, Y: 2)\n");
}

// A two-hop chain (`R(string)` -> `R(int)` -> primary) beside a direct
// `new R(3, 4)` through the primary, which must be unaffected.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           two_hop_chain_reaches_primary_spec_12_8) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(n: int) this(n, n + 1); end constructor; "
        "  public constructor(s: string) this(s.Length); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new R(\"abcde\").ToString()); "
        "    Console.WriteLine(new R(10).ToString()); "
        "    Console.WriteLine(new R(3, 4).ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "R(X: 5, Y: 6)\nR(X: 10, Y: 11)\nR(X: 3, Y: 4)\n");
}

// The #4722 comment's crash shape: a `string` positional field, stored from
// a fresh producer, across many constructions. Pre-fix this crashed; the
// value of the last construction is asserted.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           string_field_chain_loop_spec_9_1) {
    auto r = runAot(
        "module M; "
        "record P(Name: string, N: int) "
        "  public constructor(n: int) this(\"p\" + n.ToString(), n + 1); "
        "  end constructor; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var last := \"\"; "
        "    while i < 200 do "
        "      var p := new P(i); "
        "      last := p.ToString(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(last); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "P(Name: p199, N: 200)\n");
}

// §22.8 step 2 runs once, at the bottom of the chain: a body auto-property
// initializer of the combined form is still applied when the chain ends at
// the primary.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           body_initializer_runs_at_chain_bottom_spec_22_8) {
    auto r = runAot(
        "module M; "
        "record R(X: int, Y: int) "
        "  public readonly property Z: int := 7; "
        "  public constructor(a: bool) this(1, 2); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    var r := new R(true); "
        "    Console.WriteLine($\"{r.X} {r.Y} {r.Z}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1 2 7\n");
}

// A generic record: the chain stores under the record's own instantiation.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           generic_record_chain_spec_14_1) {
    auto r = runAot(
        "module M; "
        "record Box<T>(V: T, N: int) "
        "  public constructor(v: T, a: bool, b: bool) this(v, 7); "
        "  end constructor; "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var b := new Box<string>(\"hi\", true, false); "
        "    Console.WriteLine($\"{b.V} {b.N}\"); "
        "    var c := new Box<long>(5, true, true); "
        "    Console.WriteLine($\"{c.V} {c.N}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "hi 7\n5 7\n");
}

// A NULLABLE VALUE positional field (`X: int?`) and a widened one
// (`Y: long`). The shared step-0 helper stored a bare `i32` over the
// `{i8 hasValue; int value}` slot, so `X` read back as `0`; the class twin
// (`class C(x: int?, y: long)`, via a chain AND via a direct `new`) went
// through the same helper with the same result.
BEZEL_TEST(codegen_record_this_chain_primary_fields_4722,
           nullable_value_field_wrapped_spec_23_1) {
    auto r = runAot(
        "module M; "
        "record R(X: int?, Y: long) "
        "  public constructor(a: bool) this(5, 3); end constructor; "
        "end R; "
        "class C(x: int?, y: long) "
        "  public constructor(a: bool, b: bool) this(5, 3); end constructor; "
        "  public method Show(): string => $\"{x} {y}\"; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    var r := new R(true); "
        "    Console.WriteLine($\"{r.X} {r.Y}\"); "
        "    Console.WriteLine(new C(true, true).Show()); "
        "    Console.WriteLine(new C(6, 4).Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5 3\n5 3\n6 4\n");
}
