// Codegen regression tests for fix #5400 — stdlib §9.8 / §2.15 (v0.314)
// narrow integer type range checks during binary serialization deserialization.
//
// Spec reference (Bezel_Standard_Library.md §9.8, v0.314):
//   "For all integer narrower than long, the serialized form is the full 64-bit
//    value, and reading it back requires a range check: if the value is outside
//    the target type's range, SerializationException is raised instead of
//    silently truncating."
//
// The pre-fix code used LLVM's CreateTrunc which silently wraps out-of-range
// values (e.g. 300 → 44 for Byte).  Post-fix each narrow type has an explicit
// range check that raises SerializationException before the truncation.

#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const char* kImport = "module M; import System.Serialization; ";

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// §9.8 (v0.314, fix #5400): Byte range check — must raise SerializationException
// ---------------------------------------------------------------------------

// Byte is 0..255.  300 is out of range.
BEZEL_TEST(codegen_serial_narrow_range_5400, byte_300_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: byte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":300}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// Byte: -1 is below range.
BEZEL_TEST(codegen_serial_narrow_range_5400, byte_minus1_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: byte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":-1}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// Byte: 255 is at upper bound — valid.
BEZEL_TEST(codegen_serial_narrow_range_5400, byte_255_valid_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: byte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":255}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.V = 255 then return 1; end; "
        "  return 0; "
        "end Main; end C;")), 1);
}

// Byte: 0 is at lower bound — valid.
BEZEL_TEST(codegen_serial_narrow_range_5400, byte_0_valid_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: byte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":0}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.V = 0 then return 1; end; "
        "  return 0; "
        "end Main; end C;")), 1);
}

// ---------------------------------------------------------------------------
// §9.8 (v0.314, fix #5400): SByte range check.
// ---------------------------------------------------------------------------

// SByte is -128..127.  300 is out of range.
BEZEL_TEST(codegen_serial_narrow_range_5400, sbyte_300_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: sbyte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":300}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// SByte: -200 is below range.
BEZEL_TEST(codegen_serial_narrow_range_5400, sbyte_minus200_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: sbyte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":-200}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// SByte: 127 is at upper bound — valid.
BEZEL_TEST(codegen_serial_narrow_range_5400, sbyte_127_valid_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: sbyte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":127}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.V = 127 then return 1; end; "
        "  return 0; "
        "end Main; end C;")), 1);
}

// SByte: -128 is at lower bound — valid.
BEZEL_TEST(codegen_serial_narrow_range_5400, sbyte_minus128_valid_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: sbyte): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":-128}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.V = -128 then return 1; end; "
        "  return 0; "
        "end Main; end C;")), 1);
}

// ---------------------------------------------------------------------------
// §9.8 (v0.314, fix #5400): Short range check.
// ---------------------------------------------------------------------------

// Short is -32768..32767.  40000 is out of range.
BEZEL_TEST(codegen_serial_narrow_range_5400, short_40000_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: short): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":40000}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// Short: -40000 is below range.
BEZEL_TEST(codegen_serial_narrow_range_5400, short_minus40000_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: short): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":-40000}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// ---------------------------------------------------------------------------
// §9.8 (v0.314, fix #5400): UShort range check.
// ---------------------------------------------------------------------------

// UShort is 0..65535.  70000 is out of range.
BEZEL_TEST(codegen_serial_narrow_range_5400, ushort_70000_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: ushort): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":70000}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// UShort: -1 is below range.
BEZEL_TEST(codegen_serial_narrow_range_5400, ushort_minus1_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: ushort): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":-1}\"); "
        "  try "
        "    var r := R.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => return 1; "
        "  end; "
        "end Main; end C;")), 1);
}

// ---------------------------------------------------------------------------
// Control: valid values for Int and UInt at boundaries.
// ---------------------------------------------------------------------------

// Int: valid at 0.
BEZEL_TEST(codegen_serial_narrow_range_5400, int_0_valid_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":0}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.V = 0 then return 1; end; "
        "  return 0; "
        "end Main; end C;")), 1);
}

// UInt: valid at 0.
BEZEL_TEST(codegen_serial_narrow_range_5400, uint_0_valid_spec_9_8) {
    BEZEL_EXPECT_EQ(run(std::string(
        std::string(kImport) +
        "record R(V: uint): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"V\\\":0}\"); "
        "  var r := R.Deserialize(d); "
        "  if r.V = 0u then return 1; end; "
        "  return 0; "
        "end Main; end C;")), 1);
}
