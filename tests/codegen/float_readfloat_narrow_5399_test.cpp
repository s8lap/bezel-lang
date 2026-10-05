// Regression test for #5399 (stdlib §2.15 v0.314).
//
// float d.ReadFloat() synthesizes float(d.ReadFloat()) for a float field.
//
// §2.15 spec ruling: "raises SerializationException where a finite value
// does not fit" in float.  A double value that is finite but too large
// for float (e.g. 3.5e38) produces infinity via FPTrunc with no
// exception — the isfinite() check in bz_jder_parse_float already returned
// true (the double IS finite), but CreateFPTrunc silently overflows.
//
// This test verifies that deserializing a float field from a JSON number
// that is finite as double but overflows float raises SerializationException.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

BEZEL_TEST(float_readfloat_narrow_5399, json_float_field_finite_double_overflows_float_raises) {
    // §2.15 (v0.314, #5200/#5369): a JSON number whose double value is
    // finite but whose float narrowing is not finite raises
    // SerializationException — "a synthesized float read is float(d.ReadFloat())
    // with SerializationException where a finite value does not fit".
    auto r = runAot(
        "module M; "
        "import System.Serialization; "
        "record R(V: float): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 3.5e38}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := R.Deserialize(d); "
        "    return 1; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}
