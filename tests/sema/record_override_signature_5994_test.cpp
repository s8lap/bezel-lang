// #5994 (§9.2 L1228, §10 L1719, §8.1, v0.338): on a record, `override` of one of
// `Object`'s three virtual members is admitted only at the signature the member
// fills the `Object` slot with (`Equals` taking the record's own type,
// `GetHashCode(): int`, `ToString(): string`). An `override` of the same names
// at any other parameter list or return type is rejected at the declaration.
// Plain (non-override) members at those names are ordinary overloads and stay
// admitted.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// REJECT — the issue's own ToString repro: `override ToString(fmt: string)`
// matches no Object-virtual member of the record.
BEZEL_TEST(record_override_signature_5994,
           record_override_wrong_signature_tostring_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method ToString(fmt: string): string => fmt; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "record method `ToString`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "§9.2"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "§10"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// REJECT — the issue's own GetHashCode repro: `override GetHashCode(v: int)`.
BEZEL_TEST(record_override_signature_5994,
           record_override_wrong_signature_gethashcode_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method GetHashCode(v: int): int => v; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "record method `GetHashCode`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "§9.2"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// REJECT — `override Equals` at another parameter type (`Equals(other: int)`)
// is not the Object-virtual `Equals`, which takes the record's own type.
BEZEL_TEST(record_override_signature_5994,
           record_override_wrong_signature_equals_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method Equals(other: int): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "record method `Equals`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// REJECT — `override ToString(): int` keeps the name and parameter list but not
// the return type `string` of the Object-virtual `ToString` (§8.1).
BEZEL_TEST(record_override_signature_5994,
           record_override_wrong_return_tostring_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method ToString(): int => 0; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "record method `ToString`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// REJECT — `override GetHashCode(): string` keeps the parameter list but not
// the return type `int` of the Object-virtual `GetHashCode` (§8.1).
BEZEL_TEST(record_override_signature_5994,
           record_override_wrong_return_gethashcode_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method GetHashCode(): string => \"x\"; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "record method `GetHashCode`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ACCEPT — the Object-virtual signatures on a record are unchanged: `override
// ToString()`, `override GetHashCode()`, and the own-typed `override
// Equals(other: P)` all still compile and run, and the override is the one
// the program calls.
BEZEL_TEST(record_override_signature_5994,
           record_override_object_signatures_accepted_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method ToString(): string => \"P\"; "
        "  public override method GetHashCode(): int => x; "
        "  public override method Equals(other: P): bool => x = other.x; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new P(3); "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(a.GetHashCode()); "
        "    Console.WriteLine(a.Equals(new P(3))); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("P\n3\nTrue\n"));
}

// ACCEPT — plain (non-override) members named Equals / GetHashCode / ToString
// at other signatures are ordinary overloads on a record and stay admitted.
BEZEL_TEST(record_override_signature_5994,
           record_plain_other_signature_members_accepted_spec_9_2) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public method ToString(fmt: string): string => fmt; "
        "  public method GetHashCode(v: int): int => v; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new P(3); "
        "    Console.WriteLine(a.ToString(\"q\")); "
        "    Console.WriteLine(a.GetHashCode(7)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("q\n7\n"));
}

// ACCEPT — nullable return on a trio override, unchanged from base. Base accepts
// and runs these cells; the nullable-return question is held out of #5994 and
// is pending #6011 (spec §12.1), so the return type is compared without its
// nullability and these rows pin the base behaviour until #6011 rules.
BEZEL_TEST(record_override_signature_5994,
           record_override_tostring_nullable_return_unchanged_pending_6011) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method ToString(): string? => \"P\"; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new P(3); "
        "    Console.WriteLine(a.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("P\n"));
}

// ACCEPT — nullable `int?` GetHashCode return, pinned to base pending #6011.
BEZEL_TEST(record_override_signature_5994,
           record_override_gethashcode_nullable_return_unchanged_pending_6011) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method GetHashCode(): int? => x; "
        "  public override method Equals(other: P): bool => true; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new P(3); "
        "    Console.WriteLine(a.GetHashCode()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// ACCEPT — nullable `bool?` own-typed Equals return, pinned to base pending #6011.
BEZEL_TEST(record_override_signature_5994,
           record_override_equals_nullable_return_unchanged_pending_6011) {
    auto r = runAot(
        "module M; "
        "record P(x: int) "
        "  public override method Equals(other: P): bool? => x = other.x; "
        "  public override method GetHashCode(): int => x; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var a := new P(3); "
        "    Console.WriteLine(a.Equals(new P(3))); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}
