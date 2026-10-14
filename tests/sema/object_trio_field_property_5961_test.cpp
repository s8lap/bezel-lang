// #5961 (§12.2, v0.338): the field and property half. "Declaring a member in a
// derived class with the same name as any inherited member ... is a compile
// error"; `Object`'s admission "covers instance members only" at a parameter
// list other than its own, and a field or property has no parameter list, so a
// field or property named `Equals`, `GetHashCode` or `ToString` is blocked
// (instance and static alike). The `static class` case is a spec question and
// is not covered here.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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

// §12.2 (v0.338, #5961): the issue's instance-field repro.
BEZEL_TEST(object_trio_field_property_5961,
           instance_field_named_gethashcode_hides_object_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public GetHashCode: int := 0;
end C;
public class P
  public static method Main(): int Console.WriteLine("ran"); return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "field `GetHashCode` hides the inherited `Object.GetHashCode`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.338, #5961): an instance field named `ToString`.
BEZEL_TEST(object_trio_field_property_5961,
           instance_field_named_tostring_hides_object_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public ToString: int := 0;
end C;
public class P
  public static method Main(): int Console.WriteLine("ran"); return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "field `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.338, #5961): an instance property named `ToString` (no parameter
// list, so no `override` reaches the slot).
BEZEL_TEST(object_trio_field_property_5961,
           instance_property_named_tostring_hides_object_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public property ToString: int => 1;
end C;
public class P
  public static method Main(): int Console.WriteLine("ran"); return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "property `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.338, #5669/#5961): "a static member of the three names stays
// blocked" — the static field repro from #5669.
BEZEL_TEST(object_trio_field_property_5961,
           static_field_named_equals_stays_blocked_spec_12_2) {
    auto r = runAot(R"(
module P;
public class C
  public static readonly Equals: int := 1;
  public constructor()
  end constructor;
end C;
public class Program
  public static method Main()
    Console.WriteLine("ran");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "field `Equals` hides the inherited `Object.Equals`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.338, #5961): a static property named `GetHashCode`.
BEZEL_TEST(object_trio_field_property_5961,
           static_property_named_gethashcode_stays_blocked_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public static property GetHashCode: int => 1;
end C;
public class P
  public static method Main(): int Console.WriteLine("ran"); return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "property `GetHashCode` hides the inherited `Object.GetHashCode`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Accept control (§12.2, #5961): ordinary fields and properties named like
// the collection surface, a `Name` field, a `Count` property, and an instance
// `Equals` overload at another parameter list all remain admitted; a
// `Count` on a derived class is not a hiding of an Object member.
BEZEL_TEST(object_trio_field_property_5961,
           ordinary_members_named_count_name_accepted_spec_12_2) {
    auto r = runAot(R"(
module M;
public class Base
  public Count: int := 2;
  public property Label: string => "b";
end Base;
public class C: Base
  public Name: string := "c";
  public property Total: int => 7;
  public method Equals(a: int, b: int): bool => true;
end C;
public class P
  public static method Main(): int
    var c: C := new C();
    Console.WriteLine("ran");
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ran\n");
}
