// Regression test for issue #6019 (sema, spec §19.3, v0.338).
//
// §19.3 "Named destructuring": "A naked identifier on a named slot's right-hand
// side is **not** a binding. Directly after `=` — the interrogation operator —
// it resolves as at the top level of a pattern: a constant reference or a type
// test by name resolution, so `Rect(Width = w)` is an equality test when `w`
// names a `const`. The positional naked-binding shorthand is scoped to
// positional slots ... a binding in a named slot is spelled `var w`."
//
// ROOT CAUSE. Sema checked a naked identifier only at the TOP level of an arm
// (checkTopLevelNamedPattern), which also records the resolved local const for
// codegen to fold. A NAMED-slot sub-pattern never went through it, so a local
// const there was unresolved and the backend rejected it ("not supported by the
// backend"). Fix: the four destructuring slot loops (statement and expression
// `case`, choice payload and record fields) forward a named slot's NamedPattern
// to checkTopLevelNamedPattern against the slot's declared type.
//
// Every accept row asserts BOTH arms' output (matching and non-matching value)
// so a slot that is parsed and ignored cannot pass. No class row: named
// destructuring over a class head is not yet supported (#5871). Payloads are
// ints/enums (no owned payload), so no ASan leg is needed.

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

BEZEL_TEST(named_slot_naked_const_6019,
           record_field_local_const_statement_case_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a := new Rect(3, 4); var b := new Rect(5, 4); "
        "    case a of Rect(Width = W) => Console.WriteLine(\"a-eq\");, "
        "      _ => Console.WriteLine(\"a-n\"); end case; "
        "    case b of Rect(Width = W) => Console.WriteLine(\"b-eq\");, "
        "      _ => Console.WriteLine(\"b-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

BEZEL_TEST(named_slot_naked_const_6019,
           record_field_local_const_expression_case_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a := new Rect(3, 4); var b := new Rect(5, 4); "
        "    Console.WriteLine((case a of Rect(Width = W) => \"a-eq\", _ => \"a-n\")); "
        "    Console.WriteLine((case b of Rect(Width = W) => \"b-eq\", _ => \"b-n\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

BEZEL_TEST(named_slot_naked_const_6019,
           choice_payload_local_const_statement_case_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public choice Shape Sq(Side: int), Tri(Base: int, H: int); end Shape; "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a: Shape := Shape.Sq(3); var b: Shape := Shape.Sq(9); "
        "    case a of Sq(Side = W) => Console.WriteLine(\"a-eq\");, "
        "      _ => Console.WriteLine(\"a-n\"); end case; "
        "    case b of Sq(Side = W) => Console.WriteLine(\"b-eq\");, "
        "      _ => Console.WriteLine(\"b-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

BEZEL_TEST(named_slot_naked_const_6019,
           choice_payload_local_const_expression_case_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public choice Shape Sq(Side: int), Tri(Base: int, H: int); end Shape; "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var a: Shape := Shape.Sq(3); var b: Shape := Shape.Sq(9); "
        "    Console.WriteLine((case a of Sq(Side = W) => \"a-eq\", _ => \"a-n\")); "
        "    Console.WriteLine((case b of Sq(Side = W) => \"b-eq\", _ => \"b-n\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "a-eq\nb-n\n"));
}

// PIN (green at base too): a class-level const, an enum constant (bare and
// qualified) and a literal in a named slot were already equality tests.
BEZEL_TEST(named_slot_naked_const_6019,
           member_const_enum_constant_and_literal_slots_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public enum Color Red, Green, Blue; end Color; "
        "public record Rect(Width: int, Height: int); "
        "public record Pix(C: Color, N: int); "
        "public class Program "
        "  public const MW: int := 7; "
        "  public static method Main() "
        "    var a := new Rect(7, 1); var b := new Rect(5, 1); "
        "    var p := new Pix(Color.Green, 1); "
        "    case a of Rect(Width = MW) => Console.WriteLine(\"mc-eq\");, "
        "      _ => Console.WriteLine(\"mc-n\"); end case; "
        "    case b of Rect(Width = MW) => Console.WriteLine(\"mc-eq\");, "
        "      _ => Console.WriteLine(\"mc-n\"); end case; "
        "    case p of Pix(C = Green) => Console.WriteLine(\"en-eq\");, "
        "      _ => Console.WriteLine(\"en-n\"); end case; "
        "    case p of Pix(C = Red) => Console.WriteLine(\"en-eq\");, "
        "      _ => Console.WriteLine(\"en-n\"); end case; "
        "    case p of Pix(C = Color.Green) => Console.WriteLine(\"enq-eq\");, "
        "      _ => Console.WriteLine(\"enq-n\"); end case; "
        "    case b of Rect(Width = 5) => Console.WriteLine(\"lit-eq\");, "
        "      _ => Console.WriteLine(\"lit-n\"); end case; "
        "    case b of Rect(Width = 6) => Console.WriteLine(\"lit-eq\");, "
        "      _ => Console.WriteLine(\"lit-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(
        r, "mc-eq\nmc-n\nen-eq\nen-n\nenq-eq\nlit-eq\nlit-n\n"));
}

// CONTROLS. §19.3: a naked identifier in a POSITIONAL slot is a binding
// (`Rect(W, _)` binds W, so a non-matching value still selects the arm), and
// at the top level `case 3 of W` is a constant pattern.
BEZEL_TEST(named_slot_naked_const_6019,
           positional_naked_name_is_a_binding_and_top_level_const_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    const W: int := 3; "
        "    var b := new Rect(5, 4); "
        "    case b of Rect(W, _) => Console.WriteLine(\"pos\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "    case 3 of W => Console.WriteLine(\"top-eq\");, "
        "      _ => Console.WriteLine(\"top-n\"); end case; "
        "    case 4 of W => Console.WriteLine(\"top-eq\");, "
        "      _ => Console.WriteLine(\"top-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "pos\ntop-eq\ntop-n\n"));
}

// REJECT: a naked name that is not a const in a named slot is not a binding
// (§19.3) and is rejected with a located error, as it is at the top level.
BEZEL_TEST(named_slot_naked_const_6019,
           non_const_local_in_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var v := 5; var b := new Rect(5, 4); "
        "    case b of Rect(Width = v) => Console.WriteLine(\"eq\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`v` is a value"));
}

BEZEL_TEST(named_slot_naked_const_6019,
           unresolved_name_in_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var b := new Rect(5, 4); "
        "    case b of Rect(Width = Zzz) => Console.WriteLine(\"eq\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`Zzz` does not name"));
}

// REJECT: a const whose type does not match the slot's declared type.
BEZEL_TEST(named_slot_naked_const_6019,
           type_mismatched_const_in_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    const S: string := \"a\"; var b := new Rect(5, 4); "
        "    case b of Rect(Width = S) => Console.WriteLine(\"eq\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not compatible with the scrutinee type"));
}

// REJECT (behaviour flip): a naked type name in a named slot resolves as at the
// top level (§19.3 L3951), so `Box(P = Qt)` with `P: Pt` and `Qt` another
// record type is the different-value-type type pattern of §19.3 / Appendix C.1.
// Base accepted it as a dead arm (printed "n"); it is now rejected.
BEZEL_TEST(named_slot_naked_const_6019,
           other_value_type_name_in_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Pt(X: int, Y: int); "
        "public record Qt(X: int, Y: int); "
        "public record Box(P: Pt, N: int); "
        "public class Program "
        "  public static method Main() "
        "    var b := new Box(new Pt(1, 2), 3); "
        "    case b of Box(P = Qt) => Console.WriteLine(\"qt\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type pattern `Qt` is incompatible with the scrutinee's static type"));
}
