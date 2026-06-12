// Codegen tests for issue #688 — SerializationException position info.
//
// stdlib §2.15 (v0.140): "SerializationException carries a byte offset (for
// binary) or line/column (for JSON) to locate the fault."
//
// The doc says "carries" in prose without a formal property declaration block;
// the minimal conforming implementation is message inclusion.  These tests
// verify that:
//
//   1. JSON malformed input raises SerializationException whose Message
//      contains "line" and "column" (JSON: at line L, column C).
//   2. Binary truncation raises SerializationException whose Message
//      contains "byte offset".
//   3. The #668 missing-field raise still fires correctly (regression).
//   4. JSON errors on specific lines/columns produce the correct values.
//
// Spec refs: stdlib §2.15 (v0.140).
//
// Note on catch-arm syntax: per §31.2.2 / §A.6, a catch arm body is ONE of:
//   (1) an expression (no `;`), (2) a statement (with `;`), or (3) `do … end`.
// To test e.Message.Contains inside a catch arm, we use the expression-form
// assignment `hasX := (1 if e.Message.Contains("x") else 0);` which is a
// single assignment statement and parses correctly as form (2).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// §2.15 / #688: JSON malformed input — message contains line/column info
// ===========================================================================

// §2.15 / #688: JSON with unexpected token (expected int, got string) — message
// must contain "line" and "column" to satisfy the "carries line/column" contract.
// Input: `"not-a-number"` where an int is expected → ReadInt throws.
// We verify e.Message.Contains("line") AND e.Message.Contains("column").
BEZEL_TEST(codegen_serialization_688,
           json_malformed_message_contains_line_column_spec_2_15) {
    // A simple record with one int field; we feed it a JSON string value
    // instead of an integer, triggering a type-mismatch on ReadInt.
    // Uses ternary-assignment form so each check is a single statement
    // (§31.2.2 catch-arm body form 2 — assignment with `;`).
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        // Feed malformed JSON: a string where an int is expected.
        "    using var d := new JsonDeserializer(\"{\\\"X\\\": \\\"bad\\\"}\"); "
        "    var hasLine := 0; "
        "    try "
        "      var p := Pt.Deserialize(d); "
        "    catch var e: SerializationException => "
        "      hasLine := (1 if e.Message.Contains(\"line\") and e.Message.Contains(\"column\") else 0); "
        "    end; "
        "    return (42 if hasLine = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §2.15 / #688: JSON unexpected end of input — message must also carry position.
// Input is `{` alone (no closing brace, no fields).
// We only check that SerializationException is raised; position is in the message.
BEZEL_TEST(codegen_serialization_688,
           json_truncated_message_contains_position_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new JsonDeserializer(\"{\"); "
        "    var hasPos := 0; "
        "    try "
        "      var p := Pt.Deserialize(d); "
        "    catch var e: SerializationException => "
        "      hasPos := (1 if e.Message.Contains(\"line\") or e.Message.Contains(\"column\") "
        "                    or e.Message.Contains(\"byte offset\") else 0); "
        "    end; "
        "    return (42 if hasPos = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §2.15 / #688: JSON error on line 3 — verify exact line number.
// Input:
//   {
//     "X": "bad"
//   }
// The error occurs on line 2 (where "X" value is a string not an int).
// We verify e.Message.Contains("line 2").
BEZEL_TEST(codegen_serialization_688,
           json_error_on_line2_reports_line2_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        // Multi-line JSON: opening brace on line 1, field on line 2.
        "    using var d := new JsonDeserializer(\"{\\n  \\\"X\\\": \\\"bad\\\"\\n}\"); "
        "    var hasLine2 := 0; "
        "    try "
        "      var p := Pt.Deserialize(d); "
        "    catch var e: SerializationException => "
        "      hasLine2 := (1 if e.Message.Contains(\"line 2\") else 0); "
        "    end; "
        "    return (42 if hasLine2 = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// ===========================================================================
// §2.15 / #688: binary truncation — message contains "byte offset"
// ===========================================================================

// §2.15 / #688: binary truncated in the middle of an object's int field —
// message must contain "byte offset" (from bz_ser_throw_at_binary).
// We serialize Pt(X:42) and truncate the buffer to 1 byte before feeding it to
// BinaryDeserializer.  The reader hits truncation on ReadInt for field X.
BEZEL_TEST(codegen_serialization_688,
           binary_truncation_message_contains_byte_offset_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new Pt(42); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        // Use a 1-byte prefix — will always truncate the int payload
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan(0, 1)); "
        "    var hasByteOffset := 0; "
        "    try "
        "      var bad := Pt.Deserialize(de); "
        "    catch var e: SerializationException => "
        "      hasByteOffset := (1 if e.Message.Contains(\"byte offset\") else 0); "
        "    end; "
        "    return (42 if hasByteOffset = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §2.15 / #688: binary truncated at object header — message contains "byte offset".
// We pass a zero-length buffer.  begin_object immediately hits truncation.
BEZEL_TEST(codegen_serialization_688,
           binary_empty_buffer_message_contains_byte_offset_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new Pt(1); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        // Empty span → truncation at object header
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan(0, 0)); "
        "    var hasByteOffset := 0; "
        "    try "
        "      var bad := Pt.Deserialize(de); "
        "    catch var e: SerializationException => "
        "      hasByteOffset := (1 if e.Message.Contains(\"byte offset\") else 0); "
        "    end; "
        "    return (42 if hasByteOffset = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// ===========================================================================
// §2.15 / #688: regression — existing raise paths still fire correctly
// ===========================================================================

// §2.15 / #688 regression: JSON unknown field (strict mode) still raises.
// The raise now carries position but the exception must still be catchable.
BEZEL_TEST(codegen_serialization_688,
           json_unknown_field_still_raises_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new JsonDeserializer(\"{\\\"X\\\": 1, \\\"Z\\\": 99}\"); "
        "    var caught := 0; "
        "    try "
        "      var p := Pt.Deserialize(d); "
        "    catch SerializationException => caught := 1; "
        "    end; "
        "    return (42 if caught = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §9.8 / §2.15 / #668 regression: missing required field (binary) still raises.
BEZEL_TEST(codegen_serialization_688,
           binary_missing_field_still_raises_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record OneFR(X: int): ISerializable, IDeserializable; "
        "record TwoFR(X: int, Y: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new OneFR(10); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var caught := 0; "
        "    try "
        "      var bad := TwoFR.Deserialize(de); "
        "    catch SerializationException => caught := 1; "
        "    end; "
        "    return (42 if caught = 1 else 1); "
        "  end Main; "
        "end Program;"), 42);
}

// §2.15 / #688: JSON round-trip that succeeds must NOT be affected by position
// tracking (regression: skip_ws line-count must not corrupt good input).
// Uses a hand-crafted multi-line JSON string so the test exercises the newline-
// counting in bz_jder_skip_ws without relying on JsonSerializer/JsonOptions.
BEZEL_TEST(codegen_serialization_688,
           json_good_multiline_roundtrip_unaffected_spec_2_15) {
    BEZEL_EXPECT_EQ(run(
        std::string("module V; ") + kImport +
        "record Pt(X: int, Y: int): ISerializable, IDeserializable; "
        "class Program "
        "  public static method Main(): int "
        // Multi-line JSON for Pt(X=3, Y=7): {↵  "X": 3,↵  "Y": 7↵}
        // The \\n sequences become literal \n in the Bezel string.
        "    using var d := new JsonDeserializer(\"{\\n  \\\"X\\\": 3,\\n  \\\"Y\\\": 7\\n}\"); "
        "    var got := Pt.Deserialize(d); "
        "    return (42 if got.X = 3 and got.Y = 7 else 1); "
        "  end Main; "
        "end Program;"), 42);
}
