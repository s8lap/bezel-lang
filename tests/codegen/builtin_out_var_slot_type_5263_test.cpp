// Regression tests for #5263 / #5271 — an inline `out var` recipient of a
// BUILTIN member with no MethodSymbol was left UNTYPED by sema, so codegen's
// ensureOutVarSlots sized its stack slot from a name-matching ladder that
// knew only a handful of primitive receivers and defaulted everything else to
// a 4-byte i32.
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §22.4 (v0.313): "`out var name` declares a
// new variable inline. Its type is inferred from the method's `out` parameter
// declaration." The stdlib declares every member exercised here with a
// concrete out type (stdlib §1 shape (c)): Convert.TryTo* (§2.3),
// Math.DivRem (§2.2), DateTime/DateOnly/TimeOnly.TryParse (§2.5),
// Guid.TryParse (§2.6), the primitive TryParse tower (§9.1–§9.5); and the
// §1 shape-(a) collection Trys (§4.8 TryGetValue, §4.10 TryPop: `V?`/`T?`).
//
// MEASURED PRE-FIX (staging r23-c28 d68dab26, `agent_check --probe --asan`):
//   * Convert.TryToDouble / TryToInt64 / Math.DivRem(long…): ASan
//     stack-buffer-overflow, WRITE of size 8 into the 4-byte slot (#5263);
//   * Convert.TryToDecimal: WRITE of size 16 (#5263); decimal.TryParse
//     printed `125` for "1.25" and SEGV'd on a failure input (#5271);
//   * float.TryParse: malformed IR (the untyped recipient reached
//     Console.WriteLine as a bare LLVM float);
//   * sbyte.TryParse("-7") printed 249; uint.TryParse("4000000000") printed
//     -294967296; char.TryParse("a") printed 97; Convert.TryToBoolean printed 1;
//   * DateTime/DateOnly/TimeOnly/Guid.TryParse: the runtime wrote 8/16 bytes
//     through the i32 slot (uninstrumented, so no ASan report) and every
//     member read of the untyped recipient (`d.Year`) printed NOTHING.
// The rejected first fix (a name arm for Convert ahead of the `loc->declType`
// arm) additionally shadowed the #1336 collection arm: Dictionary<string,
// double>.TryGetValue / Stack<double>.TryPop out var → malformed IR, and
// Dictionary<string,long>.TryGetValue → ASan overflow. Those rows pin that
// the declType arm keeps winning for the collection Trys.
//
// The oracle is stdout byte-for-byte AND exit 0. The overflow rows need the
// ASan leg (BEZEL_AOT_FORCE_SANITIZE=address) to be observable.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string main_(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + " return 0; end Main; end C;";
}

void expectOut(const std::string& body, const std::string& want) {
    auto r = runAot(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// ---- stdlib §2.3 Convert.TryTo* -------------------------------------------

BEZEL_TEST(builtin_out_var_slot_type_5263, convert_try_to_double_inline) {
    expectOut("var ok := Convert.TryToDouble(\"2.5\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n2.5\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, convert_try_to_int64_inline) {
    expectOut("var ok := Convert.TryToInt64(\"9876543210\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n9876543210\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, convert_try_to_int32_inline) {
    expectOut("var ok := Convert.TryToInt32(\"-7\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n-7\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, convert_try_to_boolean_inline) {
    expectOut("var ok := Convert.TryToBoolean(\"true\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\nTrue\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, convert_try_to_decimal_inline) {
    expectOut("var ok := Convert.TryToDecimal(\"1.25\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n1.25\n");
}

// ---- stdlib §2.2 Math.DivRem(long, long, out long) --------------------------

BEZEL_TEST(builtin_out_var_slot_type_5263, math_divrem_long_inline) {
    expectOut("var q := Math.DivRem(9876543210L, 7L, out var r); "
              "Console.WriteLine(q); Console.WriteLine(r); "
              "var q2 := Math.DivRem(17, 5, out var r2); "
              "Console.WriteLine(q2); Console.WriteLine(r2);",
              "1410934744\n2\n3\n2\n");
}

// ---- stdlib §9.1–§9.5 primitive TryParse -----------------------------------

BEZEL_TEST(builtin_out_var_slot_type_5263, double_try_parse_inline) {
    expectOut("var ok := double.TryParse(\"2.5\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n2.5\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, float_try_parse_inline) {
    expectOut("var ok := float.TryParse(\"2.5\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n2.5\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, long_try_parse_inline) {
    expectOut("var ok := long.TryParse(\"9876543210\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n9876543210\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, ulong_try_parse_inline) {
    expectOut("var ok := ulong.TryParse(\"18446744073709551615\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n18446744073709551615\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, uint_try_parse_inline) {
    expectOut("var ok := uint.TryParse(\"4000000000\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n4000000000\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, byte_try_parse_inline) {
    expectOut("var ok := byte.TryParse(\"255\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n255\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, sbyte_try_parse_inline) {
    expectOut("var ok := sbyte.TryParse(\"-7\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n-7\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, short_try_parse_inline) {
    expectOut("var ok := short.TryParse(\"-1234\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n-1234\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, ushort_try_parse_inline) {
    expectOut("var ok := ushort.TryParse(\"65535\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n65535\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, char_try_parse_inline) {
    expectOut("var ok := char.TryParse(\"a\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\na\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, bool_try_parse_inline) {
    expectOut("var ok := bool.TryParse(\"false\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\nFalse\n");
}

// #5271: decimal.TryParse — success, negative with trailing zeros (§9.3:
// scale is not part of the value, `-0.500` renders `-0.5`), zero, and a
// failure input (out = default(T) = 0, stdlib §1 shape (c), spec §17.11).
BEZEL_TEST(builtin_out_var_slot_type_5263, decimal_try_parse_inline_5271) {
    expectOut("var ok := decimal.TryParse(\"1.25\", out var a); "
              "Console.WriteLine(ok); Console.WriteLine(a); "
              "var ok2 := decimal.TryParse(\"-0.500\", out var b); "
              "Console.WriteLine(ok2); Console.WriteLine(b); "
              "var ok3 := decimal.TryParse(\"0\", out var c); "
              "Console.WriteLine(ok3); Console.WriteLine(c); "
              "var ok4 := decimal.TryParse(\"abc\", out var e); "
              "Console.WriteLine(ok4); Console.WriteLine(e);",
              "True\n1.25\nTrue\n-0.5\nTrue\n0\nFalse\n0\n");
}

// ---- stdlib §2.5 / §2.6 value-type TryParse ---------------------------------

BEZEL_TEST(builtin_out_var_slot_type_5263, datetime_try_parse_inline) {
    expectOut("var ok := DateTime.TryParse(\"2020-01-02\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d.Year); "
              "Console.WriteLine(d.Month); Console.WriteLine(d.Day);",
              "True\n2020\n1\n2\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, dateonly_try_parse_inline) {
    expectOut("var ok := DateOnly.TryParse(\"2021-03-04\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d.Year); "
              "Console.WriteLine(d.Month); Console.WriteLine(d.Day);",
              "True\n2021\n3\n4\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, timeonly_try_parse_inline) {
    expectOut("var ok := TimeOnly.TryParse(\"13:45:10\", out var t); "
              "Console.WriteLine(ok); Console.WriteLine(t.Hour); "
              "Console.WriteLine(t.Minute); Console.WriteLine(t.Second);",
              "True\n13\n45\n10\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, guid_try_parse_inline) {
    expectOut("var ok := Guid.TryParse("
              "\"0f8fad5b-d9cb-469f-a165-70867728950e\", out var g); "
              "Console.WriteLine(ok); Console.WriteLine(g.ToString());",
              "True\n0f8fad5b-d9cb-469f-a165-70867728950e\n");
}

// ---- stdlib §1 shape (a) collection Trys (#1336 declType arm) ---------------

BEZEL_TEST(builtin_out_var_slot_type_5263, dictionary_double_try_get_value) {
    expectOut("var m := new Dictionary<string, double>(); m.Add(\"a\", 2.5); "
              "if m.TryGetValue(\"a\", out var v) then Console.WriteLine(v); end;",
              "2.5\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, dictionary_long_try_get_value) {
    expectOut("var m := new Dictionary<string, long>(); "
              "m.Add(\"a\", 9876543210L); "
              "if m.TryGetValue(\"a\", out var v) then Console.WriteLine(v); end;",
              "9876543210\n");
}

BEZEL_TEST(builtin_out_var_slot_type_5263, stack_double_try_pop) {
    expectOut("var s := new Stack<double>(); s.Push(2.5); "
              "if s.TryPop(out var v) then Console.WriteLine(v); end;",
              "2.5\n");
}
