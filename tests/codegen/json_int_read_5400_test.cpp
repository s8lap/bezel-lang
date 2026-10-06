// Codegen regression tests for fix #5400 — stdlib §2.15 (v0.314) JSON integer
// reading: fraction/exponent rejection and ERANGE checking.
//
// Spec reference (Bezel_Standard_Library.md §2.15, v0.314):
//   ReadInt reads a JSON number token and returns it as a signed integer.
//   "A value outside the representable range raises SerializationException."
//
// The pre-fix code had two defects:
//   1. No check for decimal point or exponent in the number — "5.9" was accepted
//      and silently truncated to 5; "1e3" was accepted and truncated to 1.
//   2. No ERANGE check after strtoll/strtoull — a value like "9223372036854775808"
//      (LLONG_MAX + 1) returned a wrong value instead of raising.
//
// Both defects are fixed: fraction/exponent raises SerializationException,
// and ERANGE raises SerializationException.

#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// §2.15 (v0.314, fix #5400): fraction in JSON integer — must raise.
// ---------------------------------------------------------------------------

// §2.15: ReadInt on "5.9" — fraction not allowed for integer read.
BEZEL_TEST(codegen_json_int_read_5400, fraction_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":5.9}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "    // no-throw path: FAIL
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: ReadInt on "123.456" — multi-digit fraction.
BEZEL_TEST(codegen_json_int_read_5400, fraction_multidigit_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":123.456}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// §2.15 (v0.314, fix #5400): exponent in JSON integer — must raise.
// ---------------------------------------------------------------------------

// §2.15: ReadInt on "1e3" — exponent not allowed for integer read.
BEZEL_TEST(codegen_json_int_read_5400, exponent_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":1e3}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: ReadInt on "1E+3" — uppercase E and explicit positive exponent.
BEZEL_TEST(codegen_json_int_read_5400, exponent_e_plus_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":1E+3}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// §2.15 (v0.314, fix #5400): ERANGE overflow — must raise.
// ---------------------------------------------------------------------------

// §2.15: ReadInt on "9223372036854775808" — LLONG_MAX + 1 — strtoll sets
// ERANGE.  SerializationException must be raised.
BEZEL_TEST(codegen_json_int_read_5400, int_overflow_erange_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "  // LLONG_MAX = 9223372036854775807, so LLONG_MAX+1 = 9223372036854775808\n"
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":9223372036854775808}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// §2.15: ReadUInt on "18446744073709551616" — ULONG_MAX + 1 — strtoull sets
// ERANGE.  SerializationException must be raised.
BEZEL_TEST(codegen_json_int_read_5400, uint_overflow_erange_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: uint): ISerializable, IDeserializable; "
        "  // ULONG_MAX = 18446744073709551615, so ULONG_MAX+1 = 18446744073709551616\n"
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":18446744073709551616}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Control: valid integer values are still accepted.
// ---------------------------------------------------------------------------

// §2.15: ReadInt on "123" — valid integer.
BEZEL_TEST(codegen_json_int_read_5400, valid_int_accepted_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":123}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.X = 123 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §2.15: ReadInt on "-2147483648" — INT_MIN — valid, no overflow.
// (LLONG_MIN cannot be written as a Bezel int literal; INT_MIN is sufficient
// to verify the lower-bound boundary is accepted.)
BEZEL_TEST(codegen_json_int_read_5400, valid_int_min_accepted_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":-2147483648}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.X = -2147483648 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §2.15: ReadUInt on "4294967295" — UINT_MAX — valid, no overflow.
BEZEL_TEST(codegen_json_int_read_5400, valid_uint_max_accepted_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Serialization; ") +
        "record R(X: uint): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\":4294967295}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.X = 4294967295u then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
