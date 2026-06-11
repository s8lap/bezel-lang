// Sema tests for §2.15 import-gating of BinarySerializer/BinaryDeserializer
// and constructor argument type checking (issue #631).
//
// Covers two bugs fixed by #631:
//
//   (a) Using BinarySerializer or BinaryDeserializer WITHOUT
//       `import System.Serialization;` compiled silently and produced a
//       runtime segfault. Expected: a compile error naming the missing import.
//
//   (b) `new BinaryDeserializer(bytes)` where `bytes` is the owned
//       OwnedBuffer<byte> (instead of the required ReadOnlySpan<byte>)
//       also compiled silently. Expected: a ctor-arg type error.
//
// The canonical shape — with import + AsReadOnlySpan() — must still compile
// without errors (regression guard for the existing §9.8 / #550 tests).
//
// Spec: stdlib §2.15 ("BinarySerializer/BinaryDeserializer … require
// `import System.Serialization`"), §9.8.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// ─── (a) No-import gate: BinarySerializer ────────────────────────────────────

// §2.15 / #631 (a): using BinarySerializer without import → compile error
// naming the required import.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_serializer_no_import_is_error_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "enum Color: ISerializable, IDeserializable Red, Green; end Color; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Color.Red.Serialize(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.15 / #631 (a): using BinaryDeserializer without import → compile error.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_deserializer_no_import_is_error_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "enum Color: ISerializable, IDeserializable Red, Green; end Color; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Color.Red.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // BinarySerializer and BinaryDeserializer both unimported → two errors
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.15 / #631 (a): BinarySerializer as a type annotation without import →
// compile error.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_serializer_type_annotation_no_import_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: BinarySerializer := new BinarySerializer(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.15 / #631 (a): BinaryDeserializer as a type annotation without import →
// compile error.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_deserializer_type_annotation_no_import_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "class C "
        "  public method Deser(d: BinaryDeserializer): int "
        "    return 0; "
        "  end Deser; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ─── (a) With import: both names resolve cleanly ──────────────────────────────

// §2.15 / #631 canonical form: with import, BinarySerializer usage → no error.
// (Regression guard for the existing #550 tests.)
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_serializer_with_import_accepted_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "import System.Serialization; "
        "enum Color: ISerializable, IDeserializable Red, Green; end Color; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Color.Red.Serialize(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §2.15 / #631 canonical form: with import, BinaryDeserializer usage → no
// error.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_deserializer_with_import_accepted_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "import System.Serialization; "
        "enum Color: ISerializable, IDeserializable Red, Green; end Color; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Color.Red.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var v := Color.Deserialize(d); "
        "    return (1 if v = Color.Red else 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── (b) Wrong ctor arg type: OwnedBuffer instead of ReadOnlySpan ────────────

// §2.15 / #631 (b): passing the raw OwnedBuffer<byte> to new BinaryDeserializer
// instead of the required ReadOnlySpan<byte> → ctor-arg type error.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_deserializer_owned_buffer_arg_is_error_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "import System.Serialization; "
        "enum Color: ISerializable, IDeserializable Red, Green; end Color; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Color.Red.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        // Wrong: pass bytes (OwnedBuffer<byte>) directly instead of
        // bytes.AsReadOnlySpan() (ReadOnlySpan<byte>).
        "    using var d := new BinaryDeserializer(bytes); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.15 / #631 (b): with two-arg form (ignoreUnknownFields variant) — same
// wrong type for first arg → error.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_deserializer_owned_buffer_two_arg_is_error_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "import System.Serialization; "
        "enum Color: ISerializable, IDeserializable Red, Green; end Color; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Color.Red.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes, true); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.15 / #631 (b): with import + AsReadOnlySpan() — canonical; no error.
// Uses the enum form (same as binary_deserializer_with_import_accepted_spec_2_15)
// to confirm that the AsReadOnlySpan() arg is accepted by the ctor check.
BEZEL_TEST(sema_serialization_import_gate_631,
           binary_deserializer_asreadonly_span_accepted_spec_2_15) {
    auto r = parseSema(
        "module M; "
        "import System.Serialization; "
        "enum Status: ISerializable, IDeserializable Active, Inactive; end Status; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new BinarySerializer(); "
        "    Status.Active.Serialize(s); "
        "    using var bytes := s.ToBytes(); "
        "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var v := Status.Deserialize(d); "
        "    return (1 if v = Status.Active else 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

} // namespace
