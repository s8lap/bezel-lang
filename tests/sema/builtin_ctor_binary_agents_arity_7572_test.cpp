// Spec-derived regression test for issue #7572 -- extra constructor arguments
// of the §2.15 binary agents were SILENTLY DROPPED.
//
// stdlib §2.15 (v0.355):
//   BinarySerializer()
//   BinaryDeserializer(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool := false)
//   BinaryDeserializer(input: ReadOnlyArraySpan<byte>, ignoreUnknownFields: bool := false)
// spec §17.7 (v0.355): a standard-library builtin's constructor surface is
// exactly what the standard library declares; spec §22.5: an argument list
// that matches no declared signature is a compile error.
//
// MEASURED before the fix: `new BinaryDeserializer(span, true, true)` and
// `(span, true, 42)` compiled and the extra arguments vanished.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Serialization; "
           "class C "
           "  public static method Main(): int "
           "    using var w := new BinarySerializer(); "
           "    w.WriteInt(7); "
           "    using var bytes := w.ToBytes(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectArity(const std::string& body) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(
        hasError(r, "matches no constructor the standard library declares"));
    BEZEL_EXPECT_TRUE(hasError(r, "§22.5"));
    BEZEL_EXPECT_TRUE(hasError(r, "BinaryDeserializer(input[, ignoreUnknownFields])") ||
                      hasError(r, "BinarySerializer()"));
}

}  // namespace

BEZEL_TEST(builtin_ctor_binary_agents_7572, three_args_true_true_rejected) {
    expectArity("    var x := new BinaryDeserializer(bytes.AsReadOnlySpan(), true, true); ");
}

BEZEL_TEST(builtin_ctor_binary_agents_7572, three_args_true_42_rejected) {
    expectArity("    var x := new BinaryDeserializer(bytes.AsReadOnlySpan(), true, 42); ");
}

BEZEL_TEST(builtin_ctor_binary_agents_7572, four_args_rejected) {
    expectArity("    var x := new BinaryDeserializer(bytes.AsReadOnlySpan(), true, true, true); ");
}

BEZEL_TEST(builtin_ctor_binary_agents_7572, zero_args_rejected) {
    JitResult r = runAot(prog("    var x := new BinaryDeserializer(); "));
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "required parameter `input` is not supplied"));
}

BEZEL_TEST(builtin_ctor_binary_agents_7572, serializer_with_argument_rejected) {
    JitResult r = runAot(
        "module M; import System; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var w := new BinarySerializer(1); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(
        hasError(r, "matches no constructor the standard library declares"));
    BEZEL_EXPECT_TRUE(hasError(r, "BinarySerializer()"));
}

// Every legal stdlib form keeps working and reads the value back.
BEZEL_TEST(builtin_ctor_binary_agents_7572, declared_forms_are_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
            "    using var b := new BinaryDeserializer(bytes.AsReadOnlySpan(), true); "
            "    using var c := new BinaryDeserializer(bytes.AsReadOnlySpan(), false); "
            "    using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(), "
            "ignoreUnknownFields := true); "
            "    using var e := new BinaryDeserializer(input := bytes.AsReadOnlySpan()); "
            "    Console.WriteLine($\"{a.ReadInt()} {b.ReadInt()} {c.ReadInt()} "
            "{d.ReadInt()} {e.ReadInt()}\"); ")),
        std::string("7 7 7 7 7"));
}
