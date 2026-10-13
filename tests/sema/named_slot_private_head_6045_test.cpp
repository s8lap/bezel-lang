// Regression test for issue #6045 (sema, spec §13 / §19.3 L3947, v0.338).
//
// §19.3 "Named destructuring": "a named list may name any subset of the type's
// members" and "Resolution honours ordinary member visibility (§13)". The slot
// identifier names a member of the PATTERN's type, so a private field of the
// head's record is refused whatever the scrutinee's static type is.
//
// ROOT CAUSE. #6018's visibility check keyed on the scrutinee's resolved
// declaration and admitted whenever the scrutinee was not a Named type of the
// head's own name. An Object-typed scrutinee and a `type` alias head (`R2` for
// `Rect`) therefore skipped the check, and a private field was read from
// outside its record. Fix: when the scrutinee is an Object or a record of a
// different name, the head is resolved in scope (alias followed) and the
// visibility check runs on that type.
//
// Accept rows check the runtime output so an admitted slot is actually read,
// and the same-name / choice / own-member rows guard against a false reject.

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

// REJECT: a private field of a record head, read through an Object scrutinee.
BEZEL_TEST(named_slot_private_head_6045,
           object_scrutinee_private_field_rejected_spec_13_1) {
    auto r = runAot(
        "module M; "
        "public record Rect "
        "  private h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end Rect; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := new Rect(7); "
        "    case o of Rect(h = var v) => Console.WriteLine(\"y\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`h` is private to type `Rect`"));
}

// REJECT: a private field of a record head spelled through a `type` alias,
// over a scrutinee of the aliased record type.
BEZEL_TEST(named_slot_private_head_6045,
           alias_head_private_field_rejected_spec_13_1) {
    auto r = runAot(
        "module M; "
        "public record Rect "
        "  private h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end Rect; "
        "type R2 := Rect; "
        "public class Program "
        "  public static method Main() "
        "    var r: R2 := new Rect(7); "
        "    case r of R2(h = var v) => Console.WriteLine(\"y\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`h` is private to type `Rect`"));
}

// REJECT: the module-qualified alias head (`M.R2(...)`) over an Object
// scrutinee, the same head resolved through the module's own name.
BEZEL_TEST(named_slot_private_head_6045,
           qualified_alias_head_object_scrutinee_private_rejected_spec_13_1) {
    auto r = runAot(
        "module M; "
        "public record Rect "
        "  private h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end Rect; "
        "type R2 := Rect; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := new Rect(7); "
        "    case o of M.R2(h = var v) => Console.WriteLine(\"y\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`h` is private to type `Rect`"));
}

// REJECT: the generic form of the alias head (`type BI := Box<int>`).
BEZEL_TEST(named_slot_private_head_6045,
           generic_alias_head_object_scrutinee_private_rejected_spec_13_1) {
    auto r = runAot(
        "module M; "
        "public record Box<T> "
        "  private h: T; "
        "  public constructor(x: T) h := x; end constructor; "
        "end Box; "
        "type BI := Box<int>; "
        "public class Program "
        "  public static method Main() "
        "    var o: Object := new Box<int>(7); "
        "    case o of BI(h = var v) => Console.WriteLine(\"y\");, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`h` is private to type `Box`"));
}

// ACCEPT (guard): a PUBLIC field through an alias head and an Object scrutinee
// is admitted and read.
BEZEL_TEST(named_slot_private_head_6045,
           alias_head_public_field_accepted_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record PubRec "
        "  public h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "end PubRec; "
        "type R3 := PubRec; "
        "public class Program "
        "  public static method Main() "
        "    var r: R3 := new PubRec(7); "
        "    case r of R3(h = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "    var o: Object := new PubRec(8); "
        "    case o of PubRec(h = 8) => Console.WriteLine(\"obj-eq\");, "
        "      _ => Console.WriteLine(\"obj-n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "7\nobj-eq\n"));
}

// ACCEPT (guard): inside the record's own member, an alias head over an Object
// scrutinee reads its own private field (§13: visible to the declaring type).
BEZEL_TEST(named_slot_private_head_6045,
           alias_head_private_field_inside_own_member_accepted_spec_13_1) {
    auto r = runAot(
        "module M; "
        "public record Rect "
        "  private h: int; "
        "  public constructor(x: int) h := x; end constructor; "
        "  public method Test(): string "
        "    var o: Object := this; "
        "    case o of R2(h = var v) => return \"in\" + v.ToString();, "
        "      _ => return \"o\"; end case; "
        "  end Test; "
        "end Rect; "
        "type R2 := Rect; "
        "public class Program "
        "  public static method Main() "
        "    var r := new Rect(7); "
        "    Console.WriteLine(r.Test()); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "in7\n"));
}

// ACCEPT (guard): two same-named types, A.Item with a PRIVATE field and B.Item
// with a PUBLIC field. Inside B the head `Item` names B.Item, so the public
// slot is admitted over an Object scrutinee.
BEZEL_TEST(named_slot_private_head_6045,
           same_name_nested_head_resolves_in_scope_accepted_spec_19_3) {
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
        "  public static method Main() "
        "    var o: Object := new Item(5); "
        "    case o of Item(h = 5) => Console.WriteLine(\"m5\");, "
        "      _ => Console.WriteLine(\"o\"); end case; "
        "  end Main; "
        "end B;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "m5\n"));
}

// ACCEPT (guard): a choice case whose name is also a record type in scope. The
// head names the CHOICE CASE over a choice scrutinee, so the record's private
// field is not the slot's member and the case payload is read.
BEZEL_TEST(named_slot_private_head_6045,
           choice_case_head_shadowing_record_name_accepted_spec_35_5) {
    auto r = runAot(
        "module M; "
        "public record Sq "
        "  private Side: int; "
        "  public constructor(x: int) Side := x; end constructor; "
        "end Sq; "
        "public choice Shape Sq(Side: int), Tri(Base: int); end Shape; "
        "public class Program "
        "  public static method Main() "
        "    var s: Shape := Shape.Sq(4); "
        "    case s of Sq(Side = var v) => Console.WriteLine(v.ToString());, "
        "      _ => Console.WriteLine(\"n\"); end case; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outIs(r, "4\n"));
}
