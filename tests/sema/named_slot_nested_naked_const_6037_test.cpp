// Regression test for issue #6037 (sema, spec §19.3, v0.338).
//
// §19.3 "Named destructuring" (L3951): "A naked identifier on a named slot's
// right-hand side is **not** a binding. Directly after `=` it resolves as at
// the top level of a pattern: a constant reference or a type test by name
// resolution". This applies at every depth, so `Outer(I = Inner(X = W))` with a
// local `const W` is an equality test on the nested slot.
//
// ROOT CAUSE. #6019 forwarded a named slot's NamedPattern to
// checkTopLevelNamedPattern in the four TOP-LEVEL destructuring loops only.
// The nested walk (bindNestedDestructuringNames) never did, so a local const
// under a nested named slot was unresolved and the backend rejected it ("not
// supported by the backend"). Fix: the nested walk calls checkNamedSlotNakedName
// on each sub-pattern, the same helper the top-level loops use.
//
// Every accept row asserts BOTH arms' output (matching and non-matching value)
// so a slot that is parsed and ignored cannot pass. Payloads are ints, so no
// ASan leg is needed.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool outIs(const bezel::test::JitResult& r, const std::string& expected) {
    return r.stdoutText == expected || [&] {
        std::string crlf;
        for (char c : expected) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        return r.stdoutText == crlf;
    }();
}
}  // namespace

BEZEL_TEST(named_slot_nested_naked_const_6037,
           nested_record_named_slot_local_const_statement_case_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Inner(X: int); "
        "public record Outer(I: Inner); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a := new Outer(new Inner(3)); var b := new Outer(new Inner(5)); "
        "    case a of Outer(I = Inner(X = W)) => Console.WriteLine(\"a-eq\");, "
        "      _ => Console.WriteLine(\"a-n\"); end case; "
        "    case b of Outer(I = Inner(X = W)) => Console.WriteLine(\"b-eq\");, "
        "      _ => Console.WriteLine(\"b-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

BEZEL_TEST(named_slot_nested_naked_const_6037,
           nested_record_named_slot_local_const_expression_case_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Inner(X: int); "
        "public record Outer(I: Inner); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a := new Outer(new Inner(3)); var b := new Outer(new Inner(5)); "
        "    Console.WriteLine((case a of Outer(I = Inner(X = W)) => \"a-eq\", _ => \"a-n\")); "
        "    Console.WriteLine((case b of Outer(I = Inner(X = W)) => \"b-eq\", _ => \"b-n\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

// A nested choice payload under a named slot: `Outer(I = Sq(Side = W))`.
BEZEL_TEST(named_slot_nested_naked_const_6037,
           nested_choice_payload_named_slot_local_const_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public choice Shape Sq(Side: int), Tri(Base: int, H: int); end Shape; "
        "public record Outer(I: Shape, N: int); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a := new Outer(Shape.Sq(3), 1); var b := new Outer(Shape.Sq(9), 1); "
        "    case a of Outer(I = Sq(Side = W)) => Console.WriteLine(\"a-eq\");, "
        "      _ => Console.WriteLine(\"a-n\"); end case; "
        "    case b of Outer(I = Sq(Side = W)) => Console.WriteLine(\"b-eq\");, "
        "      _ => Console.WriteLine(\"b-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

// CONTROL: a positional nested slot is still a binding (§19.3): `Inner(W)`
// binds W, so every value selects the arm.
BEZEL_TEST(named_slot_nested_naked_const_6037,
           nested_positional_naked_name_is_a_binding_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Inner(X: int); "
        "public record Outer(I: Inner); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var b := new Outer(new Inner(5)); "
        "    case b of Outer(I = Inner(W)) => Console.WriteLine(\"bind\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "bind\n"));
}

// REJECT: a non-const local under a nested named slot is not a binding and is
// rejected with a located error, as it is at the top level and one level up
// (#6019).
BEZEL_TEST(named_slot_nested_naked_const_6037,
           nested_non_const_local_in_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Inner(X: int); "
        "public record Outer(I: Inner); "
        "public class Program "
        "  public static method Main() "
        "    var v := 5; var b := new Outer(new Inner(5)); "
        "    case b of Outer(I = Inner(X = v)) => Console.WriteLine(\"eq\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`v` is a value"));
}
