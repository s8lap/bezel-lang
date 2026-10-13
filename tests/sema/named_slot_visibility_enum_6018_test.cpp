// Spec-derived tests for §19.3 "Named destructuring" (v0.338, #6018): the
// named-slot head rules that the named-slot normalization did not enforce.
//
// §19.3, "Named destructuring" (spec v0.338):
//   "A named slot's `Identifier` resolves against the pattern's type, by kind:"
//   ... "Record — ... or a public explicit field" ...
//   "... a record **method** or non-`public` explicit field, or — on a class —
//    a field or method rather than a property, is a compile error at the
//    pattern, each naming its rule. Enums and primitives have no
//    named-destructure form (they have no slot surface). An interface-typed
//    pattern head is not admitted in v1. Resolution honours ordinary member
//    visibility ([§13]); v1 fixes the class set to *public* readable
//    properties."
//
// Before #6018 a named slot on a PRIVATE property (a record head whose
// property is not in the FieldSymbol list the normalization reads) and an
// enum head (whose empty member list skipped the normalization entirely) both
// compiled and ran. Both are refused now, by the sema rule in
// `ExprTC::rejectUnadmittedNamedSlotHead`.
//
// Shape of the file:
//   - REJECT rows: a private property, a private explicit field, a protected
//     explicit field, an enum head. Each must fail at compile time with a
//     located diagnostic naming the rule.
//   - ACCEPT + RUN rows: a public explicit field, an internal explicit field in
//     the same module (§13.4), a choice-case head, a record head's positional
//     slot. Each must run and print the expected value; these pin that the
//     refusal above is not over-broad.
//   - A SEMA-ONLY control: a PUBLIC body property named slot is accepted. It is
//     compile-only on purpose: a body auto-property named slot does not match
//     at run time (#6017, codegen, out of scope here), so the control asserts
//     the sema verdict and does not run the program.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot — the real bezelc AOT oracle
#include "support/sema_support.hpp"         // parseAndTypecheckGated
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndTypecheckGated;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — §19.3 "Resolution honours ordinary member visibility" (§13).
// The headline repro: a private PROPERTY named as a slot. Program is in the
// same module but is not Rect6018, so `Hidden` is private to Rect6018.
// ===========================================================================
BEZEL_TEST(named_slot_visibility_enum_6018,
           private_property_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018 "
        "  private property Hidden: int; "
        "  public constructor(h: int) Hidden := h; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    case r of "
        "      Rect6018(Hidden = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "`Hidden` is private to type `Rect6018`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§13.1"));
}

// The same rule on an explicit private FIELD: the named slot on a non-public
// explicit field was never refused (the normalization ignores visibility).
BEZEL_TEST(named_slot_visibility_enum_6018,
           private_explicit_field_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018 "
        "  private h: int; "
        "  public w: int; "
        "  public constructor(x: int) h := x; w := 2; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    case r of "
        "      Rect6018(h = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "`h` is private to type `Rect6018`"));
}

// The same refusal in EXPRESSION position: a parenthesized `case` expression
// is a second caller of the record normalization (ExprTypecheck_Exprs.cpp),
// so the private field is refused there too.
BEZEL_TEST(named_slot_visibility_enum_6018,
           case_expression_private_field_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018 "
        "  private h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    var s: string := (case r of Rect6018(h = var v) => \"y\", _ => \"n\"); "
        "    Console.WriteLine(s); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "`h` is private to type `Rect6018`"));
}

// `protected` is visible to the declaring class and its derived classes only;
// Program is neither, so the named slot is refused (§13.1).
BEZEL_TEST(named_slot_visibility_enum_6018,
           protected_explicit_field_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018 "
        "  protected h: int; "
        "  public w: int; "
        "  public constructor(x: int) h := x; w := 2; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    case r of "
        "      Rect6018(h = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "`h` is protected on type `Rect6018`"));
}

// ===========================================================================
// REJECT — §19.3 "Enums and primitives have no named-destructure form".
// `Color(X = var v)` on an enum scrutinee: there is no slot surface to name.
// ===========================================================================
BEZEL_TEST(named_slot_visibility_enum_6018,
           enum_head_named_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public enum Color6018 Red6018, Blue6018; end Color6018; "
        "public class Program "
        "  public static method Main() "
        "    var c: Color6018 := Color6018.Red6018; "
        "    case c of "
        "      Color6018(X = var v) => Console.WriteLine(\"matched\");, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "enums have no named-destructure form"));
}

// ===========================================================================
// ACCEPT + RUN — the controls. None of these may be refused by #6018.
// ===========================================================================

// A PUBLIC explicit field named slot on a record: the admitted surface that
// the refusal above must leave alone. Prints 7.
BEZEL_TEST(named_slot_visibility_enum_6018,
           public_explicit_field_named_slot_accepted_and_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018 "
        "  public h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    case r of "
        "      Rect6018(h = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// §13.4: `internal` is visible within its own module, so an internal explicit
// field named by a same-module record pattern is admitted. Prints 7.
BEZEL_TEST(named_slot_visibility_enum_6018,
           internal_explicit_field_named_slot_same_module_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018 "
        "  internal h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    case r of "
        "      Rect6018(h = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// A CHOICE-case head with a named payload slot is not a record and not an
// enum: it stays admitted and runs. Prints 42.
BEZEL_TEST(named_slot_visibility_enum_6018,
           choice_case_head_named_slot_accepted_and_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public choice Opt6018 Some6018(value: int), None6018; end Opt6018; "
        "public class Program "
        "  public static method Main() "
        "    var o: Opt6018 := Opt6018.Some6018(42); "
        "    case o of "
        "      Some6018(value = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// A record head's POSITIONAL slot, named by its positional field name, is the
// §9.1 auto-property surface and stays admitted. Prints 4.
BEZEL_TEST(named_slot_visibility_enum_6018,
           record_positional_slot_named_accepted_and_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect6018(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(3, 4); "
        "    case r of "
        "      Rect6018(Width = 3, Height = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// ===========================================================================
// ACCEPT + RUN — SAME-NAME nested types. The pattern head `Item` must resolve
// to the SCRUTINEE's type (B.Item here), never to a same-named type elsewhere
// (A.Item). A short-name lookup refused these (a private A.Item field and an
// enum A.Item were named instead of B.Item); the scrutinee type is the
// one the match tests against. Prints m5. The slot is a CONSTANT (`h = 5`),
// not `var v`: a binding of a same-named NESTED type's field fails to lower
// (warning #3079, "the argument ... failed to lower"), identically on the
// pre-fix compiler, so a `var` row would pin that defect. A constant refutes
// ("o") when the wrong type is read, so the m5 row still distinguishes.
// ===========================================================================
BEZEL_TEST(named_slot_visibility_enum_6018,
           same_name_nested_record_public_slot_accepted_and_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public class A "
        "  public record Item "
        "    private h: int; "
        "    public constructor(x: int) h := x; end constructor; "
        "  end Item; "
        "end A; "
        "public class B "
        "  public record Item "
        "    public h: int; "
        "    public constructor(x: int) h := x; end constructor; "
        "  end Item; "
        "  public static method Make(): Item return new Item(5); end Make; "
        "end B; "
        "public class Program "
        "  public static method Main() "
        "    var i := B.Make(); "
        "    case i of "
        "      Item(h = 5) => Console.WriteLine(\"m5\");, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "m5\n");
}

// The NEGATIVE half of the same-name rule: the scrutinee is A.Item, whose
// field IS private, so the refusal must still fire on the scrutinee's type
// (the nested type is reached through the scrutinee, not a name scan).
BEZEL_TEST(named_slot_visibility_enum_6018,
           nested_scrutinee_private_slot_rejected_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public class A "
        "  public record Item "
        "    private h: int; "
        "    public constructor(x: int) h := x; end constructor; "
        "  end Item; "
        "  public static method Make(): Item return new Item(5); end Make; "
        "end A; "
        "public class Program "
        "  public static method Main() "
        "    var i := A.Make(); "
        "    case i of "
        "      Item(h = 5) => Console.WriteLine(\"m5\");, "
        "      _ => Console.WriteLine(\"o\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "`h` is private to type `Item`"));
}

// Same-name shape with an ENUM A.Item and a record B.Item: the scrutinee is the
// record, so the enum-head refusal must not fire on the name alone. Prints m5.
BEZEL_TEST(named_slot_visibility_enum_6018,
           same_name_enum_elsewhere_record_head_accepted_and_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public class A "
        "  public enum Item X, Y; end Item; "
        "end A; "
        "public class B "
        "  public record Item "
        "    public h: int; "
        "    public constructor(x: int) h := x; end constructor; "
        "  end Item; "
        "  public static method Make(): Item return new Item(5); end Make; "
        "end B; "
        "public class Program "
        "  public static method Main() "
        "    var i := B.Make(); "
        "    case i of "
        "      Item(h = 5) => Console.WriteLine(\"m5\");, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "m5\n");
}

// A QUALIFIED head `B.Item(h = var v)` names the same record the scrutinee
// has; the last path part resolves through the scrutinee type. Prints m5.
BEZEL_TEST(named_slot_visibility_enum_6018,
           qualified_head_same_name_nested_record_accepted_and_runs_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public class A "
        "  public record Item "
        "    private h: int; "
        "    public constructor(x: int) h := x; end constructor; "
        "  end Item; "
        "end A; "
        "public class B "
        "  public record Item "
        "    public h: int; "
        "    public constructor(x: int) h := x; end constructor; "
        "  end Item; "
        "  public static method Make(): Item return new Item(5); end Make; "
        "end B; "
        "public class Program "
        "  public static method Main() "
        "    var i := B.Make(); "
        "    case i of "
        "      B.Item(h = 5) => Console.WriteLine(\"m5\");, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "m5\n");
}

// SEMA-ONLY CONTROL (not run). A PUBLIC body property named slot is a
// §19.3 admitted target ("a public readable property", the record bullet's
// body auto-property). Its sema verdict must stay clean. The program is NOT run:
// a body auto-property named slot does not match at run time (#6017, codegen,
// filed separately), so asserting a run here would pin a wrong value.
BEZEL_TEST(named_slot_visibility_enum_6018,
           public_body_property_named_slot_sema_accepts_spec_19_3) {
    auto r = parseAndTypecheckGated(
        "module M; "
        "public record Rect6018 "
        "  public property Hidden: int; "
        "  public constructor(h: int) Hidden := h; end constructor; "
        "end Rect6018; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect6018(7); "
        "    case r of "
        "      Rect6018(Hidden = var h) => Console.WriteLine(h.ToString());, "
        "      _ => Console.WriteLine(\"other\"); "
        "    end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
