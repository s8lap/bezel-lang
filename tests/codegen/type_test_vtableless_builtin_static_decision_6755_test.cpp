// Codegen — spec §12.7 / §17.4, v0.339 — issue #6755 (narrow): a NON-GENERIC
// vtable-less builtin class target (StringBuilder, Random, ...) has no runtime
// descriptor, so `lowerTypeTest` bailed silently and `sb is StringBuilder` /
// `rn is Random` / `sb as StringBuilder` degraded to the #2434 / #3079
// "failed to lower" warnings (the whole `Console.WriteLine` statement emitted
// nothing). When the OPERAND's static type is itself such a sealed builtin the
// answer is static: same head and non-null -> true, otherwise false.
//
// NOT covered (stays open under #6755): an Object/interface carrier
// (`o: Object := new StringBuilder(); o is StringBuilder`), which needs a
// runtime type tag on vtable-less builtins (the #5059 gap).
//
// MEASURED fail-without: on 997824b6 each row below emits no line / the
// wrong value (see the lane handback).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// §12.7: a StringBuilder-typed operand tested against StringBuilder is true;
// against Random false. Pre-fix both statements emitted nothing.
BEZEL_TEST(codegen_type_test_6755,
           stringbuilder_operand_is_stringbuilder_not_random_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    Console.WriteLine(sb is StringBuilder); "
        "    Console.WriteLine(sb is Random); "
        "    Console.WriteLine(sb is not Random); "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("True\nFalse\nTrue\nafter"));
}

// §12.7: the issue's Random line, with the receiver statically Random.
BEZEL_TEST(codegen_type_test_6755,
           random_operand_is_random_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var r := new Random(1); "
        "    var t: bool := r is Random; "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(r is StringBuilder); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("True\nFalse"));
}

// §12.7 / §12.7 `as`: "returns the value cast to the target type, or null if
// the cast fails" -- a matching `as` is non-null, a mismatched one null; and
// §12.7 "null is an instance of no type" for a nullable operand.
BEZEL_TEST(codegen_type_test_6755,
           as_and_null_operand_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    var same := sb as StringBuilder; "
        "    Console.WriteLine(same = null); "
        "    var other := sb as Random; "
        "    Console.WriteLine(other = null); "
        "    var n: StringBuilder? := null; "
        "    Console.WriteLine(n is StringBuilder); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("False\nTrue\nFalse"));
}
