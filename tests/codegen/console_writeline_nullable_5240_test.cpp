// Regression test for #5240 — `Console.WriteLine(q)` with `q: T?` (a nullable
// type parameter) inside a generic body, instantiated at a value type, passes
// the nullable carrier `{i8,T}` to `bz_string_to_utf8z`, causing an IR verify
// failure.
//
// WHAT THE SPEC REQUIRES. stdlib §2.1 Console: `WriteLine(value: string)`,
// `WriteLine(value: Object)` and per-type bool/numeric overloads. "A `null`
// argument to an `Object` overload renders as the four characters `null`."
// Expected: value prints the payload; null prints `null`. Which overload a
// call selects is §22.6 overloading's business; the renderings asserted here
// are the ones stdlib §2.1 fixes.
//
// THE BUG PATH (generic body, value-type instantiation). `tryLowerConsoleCall`
// intercepts a nullable VALUE argument before the primitive dispatch: it
// branches on the carrier's `hasValue` tag and formats the payload via
// `emitValueToString`, or emits the literal `null`. That interception has to
// recognize the carrier from the argument's recorded SEMANTIC type — the value
// is lowered once, on the weakest assumption the generic's constraints
// establish, and `T` is substituted when each instantiation is judged (§3.6
// rule 2, v0.313). So the carrier shows up under either a concrete nullable
// type (`int?`, `bool?`, `E?`, `R?`) or a still-bare `T`.
//
// An earlier cut recognized it from the SHAPE of the lowered LLVM value —
// "a 2-element struct whose element 0 is the `i8` hasValue tag and whose
// element 1 is not a pointer". That shape is not exclusive to a nullable
// carrier: `lowerPrimitive` maps `byte`/`sbyte` to `i8`, so an ordinary
// non-nullable `record Q(b: byte, n: int)` lowers to exactly `{i8, i32}`.
// Such a record was routed into the unwrap arm, where the payload was
// extracted as element 1 while `*argTy` still named the RECORD — so
// `Console.WriteLine(q)` printed `99` instead of `Q(b: 7, n: 99)`: a wrong
// value, exit status 0, no diagnostic. `record_byte_int_value_type` below is
// that exact repro, asserted on the printed output.
//
// SPEC-CITED TEST CASES (stdlib §2.1 / §7.2 / §22.6, v0.313):
//   T=int: value → "5", null → "null"
//   T=long: value → "42", null → "null"
//   T=bool: value → "True"/"False", null → "null"
//   T=string (reference): value → the string
//   T=record (value): value → record render, null → "null"
//   T=enum (value): value → enum name, null → "null"

// #6778 (owner ruling, §7.1, §5.1, §22.6, v0.340): every one-argument
// `WriteLine` overload is non-nullable, so an un-narrowed `T?` argument is a
// compile error. The generic bodies below therefore narrow `q` before the call
// (the carrier still reaches the console lowering, now narrowed) and print the
// absent case as the literal `null`; the expected stdout is unchanged. The old
// un-narrowed spelling is a reject row in
// tests/codegen/console_nullable_arg_6778_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// T=int, non-null and null — the primary IR-verify failure case.
const char* kIntProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    // #6778 (§7.1, §22.6, v0.340): an un-narrowed `T?` has no applicable
    // `WriteLine` overload, so the carrier reaches the call narrowed.
    "    if q <> null then Console.WriteLine(q); "
    "    else Console.WriteLine(\"null\"); end; "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    var p: int? := 5; "
    "    G.Show<int>(p); "
    "    return 0; "
    "  end Main; "
    "end P;";

// T=long.
const char* kLongProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    // #6778 (§7.1, §22.6, v0.340): an un-narrowed `T?` has no applicable
    // `WriteLine` overload, so the carrier reaches the call narrowed.
    "    if q <> null then Console.WriteLine(q); "
    "    else Console.WriteLine(\"null\"); end; "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    var p: long? := 42; "
    "    G.Show<long>(p); "
    "    return 0; "
    "  end Main; "
    "end P;";

// T=bool, both true and false, and null.
const char* kBoolProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    // #6778 (§7.1, §22.6, v0.340): an un-narrowed `T?` has no applicable
    // `WriteLine` overload, so the carrier reaches the call narrowed.
    "    if q <> null then Console.WriteLine(q); "
    "    else Console.WriteLine(\"null\"); end; "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    G.Show<bool>(true); "
    "    G.Show<bool>(false); "
    "    var n: bool? := null; "
    "    G.Show<bool>(n); "
    "    return 0; "
    "  end Main; "
    "end P;";

// The issue asks for `T = int, long, bool, string, record, enum`, each with a
// value and with null. The generic `T?` rows below cover int, long, bool and
// string; the record and enum instantiations — plus the NON-generic
// `record(byte, int)` repro of the miscompile — are the programs that follow.

// Control: non-generic int? (the pre-existing, always-correct behaviour).
const char* kNonGenericIntProgram =
    "module M; "
    "class P "
    "  public static method Main(): int "
    "    var q: int? := 5; "
    "    Console.WriteLine(q); "
    "    var n: int? := null; "
    // #6778 (§7.1, §22.6, v0.340): the absent value prints through a hole.
    "    Console.WriteLine($\"{n}\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

// The #5240 miscompile repro, verbatim: a NON-generic, NON-nullable
// `record(byte, int)` printed through Console.WriteLine. `byte` lowers to
// `i8`, so this record lowers to `{i8,i32}` — the same LLVM shape as a
// nullable-value carrier — and the structural detection routed it into the
// carrier unwrap. Expected rendering is the record's own (§9.4 record
// `ToString`), NOT the `n` field alone.
const char* kByteIntRecordProgram =
    "module M; "
    "record R(b: byte, n: int); "
    "class P "
    "  public static method Main(): int "
    "    var r := new R(7, 99); "
    "    Console.WriteLine(r); "
    "    return 0; "
    "  end Main; "
    "end P;";

// Control for the row above: the SAME program with `b: short` (i16, so not
// the carrier shape). The two records must print identically.
const char* kShortIntRecordProgram =
    "module M; "
    "record R(b: short, n: int); "
    "class P "
    "  public static method Main(): int "
    "    var r := new R(7, 99); "
    "    Console.WriteLine(r); "
    "    return 0; "
    "  end Main; "
    "end P;";

// Non-generic nullable VALUE types across the value/primitive boundary —
// the payloads the carrier unwrap must render rather than the hasValue tag.
const char* kNonGenericValueProgram =
    "module M; "
    "record Q(b: int, n: int); "
    "enum Color Red, Green, Blue; end Color; "
    "class P "
    "  public static method Main(): int "
    "    var q := new Q(7, 99); "
    "    Console.WriteLine(q); "
    "    var e: Color := Color.Green; "
    "    Console.WriteLine(e); "
    "    var s: string := \"hello\"; "
    "    Console.WriteLine(s); "
    "    var i: int? := 5; "
    "    Console.WriteLine(i); "
    "    var l: long? := 42; "
    "    Console.WriteLine(l); "
    "    return 0; "
    "  end Main; "
    "end P;";

// The generic `T?` body at the record and enum type arguments, each with a
// value and with null — the instantiations the issue asks for that the
// earlier rows did not reach.
const char* kGenericRecordEnumProgram =
    "module M; "
    "record Q(b: int, n: int); "
    "enum Color Red, Green, Blue; end Color; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    // #6778 (§7.1, §22.6, v0.340): an un-narrowed `T?` has no applicable
    // `WriteLine` overload, so the carrier reaches the call narrowed.
    "    if q <> null then Console.WriteLine(q); "
    "    else Console.WriteLine(\"null\"); end; "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    var a: Q? := new Q(7, 99); "
    "    G.Show<Q>(a); "
    "    var an: Q? := null; "
    "    G.Show<Q>(an); "
    "    var c: Color? := Color.Green; "
    "    G.Show<Color>(c); "
    "    var cn: Color? := null; "
    "    G.Show<Color>(cn); "
    "    return 0; "
    "  end Main; "
    "end P;";

// The generic `T?` body at `T = string` (a REFERENCE type argument — the one
// instantiation whose payload is a pointer, so the carrier is not a
// `{i8,T}` value struct at all).
const char* kGenericStringProgram =
    "module M; "
    "class G "
    "  public static method Show<T>(v: T?) "
    "    var q: T? := v; "
    // #6778 (§7.1, §22.6, v0.340): an un-narrowed `T?` has no applicable
    // `WriteLine` overload, so the carrier reaches the call narrowed.
    "    if q <> null then Console.WriteLine(q); "
    "    else Console.WriteLine(\"null\"); end; "
    "  end Show; "
    "end G; "
    "class P "
    "  public static method Main(): int "
    "    var s: string? := \"strv\"; "
    "    G.Show<string>(s); "
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// §3.6 rule 2 / stdlib §2.1 / #5240: T=int, value-type instantiation,
// primary IR-verify case.
BEZEL_TEST(console_writeline_nullable_5240, int_value_type_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kIntProgram), "5");
}

// §3.6 rule 2 / stdlib §2.1 / #5240: T=long.
BEZEL_TEST(console_writeline_nullable_5240, long_value_type_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kLongProgram), "42");
}

// §3.6 rule 2 / stdlib §2.1 / #5240: T=bool, true / false / null.
BEZEL_TEST(console_writeline_nullable_5240, bool_value_type_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kBoolProgram), "True\nFalse\nnull");
}

// #5240 control: non-generic int? — pre-existing behaviour must stay exact.
BEZEL_TEST(console_writeline_nullable_5240, non_generic_int_control) {
    BEZEL_EXPECT_EQ(runAotOut(kNonGenericIntProgram), "5\nnull");
}

// #5240 miscompile: a non-nullable `record(byte, int)` must render as the
// record. Under the structural detection this printed `99` — the second
// field alone — with exit status 0 and no diagnostic.
BEZEL_TEST(console_writeline_nullable_5240, record_byte_int_value_type) {
    BEZEL_EXPECT_EQ(runAotOut(kByteIntRecordProgram), "R(b: 7, n: 99)");
}

// Control for the row above: `record(short, int)` lowers to `{i16,i32}`,
// which never matched the carrier shape. It must keep printing the record —
// and identically to the `byte` spelling.
BEZEL_TEST(console_writeline_nullable_5240, record_short_int_value_type) {
    BEZEL_EXPECT_EQ(runAotOut(kShortIntRecordProgram), "R(b: 7, n: 99)");
}

// stdlib §2.1 / §7.2 / #5240: the value renderings the carrier unwrap must
// produce for a record, an enum, a string, an `int?` and a `long?`.
BEZEL_TEST(console_writeline_nullable_5240, value_type_renderings) {
    BEZEL_EXPECT_EQ(runAotOut(kNonGenericValueProgram),
                    "Q(b: 7, n: 99)\nM.Color\nhello\n5\n42");
}

// §3.6 rule 2 / stdlib §2.1 / #5240: the generic body at T=record and
// T=enum, each with a value and with null.
BEZEL_TEST(console_writeline_nullable_5240, generic_record_enum_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kGenericRecordEnumProgram),
                    "Q(b: 7, n: 99)\nnull\nM.Color\nnull");
}

// §3.6 rule 2 / stdlib §2.1 / #5240: the generic body at T=string — the
// reference-type argument, whose payload is a pointer.
BEZEL_TEST(console_writeline_nullable_5240, generic_string_instantiation) {
    BEZEL_EXPECT_EQ(runAotOut(kGenericStringProgram), "strv");
}
