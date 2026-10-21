// fix #6765 (§12.2, v0.340): "Declaring a member in a derived class with the
// same name as any inherited member ... is a compile error", and a nested
// type is a member (§11.6). A nested type named `ToString`, `GetHashCode` or
// `Equals` hides the inherited `Object` member and is rejected; the rejection
// was right and only the kind word was wrong: the message called the nested
// type a "field". Its kind word now matches the declaration (the `GetType` /
// `ReferenceEquals` path of #6729 already said "nested type").
//
// Probes: /var/tmp/bezel-scratch/r34-diag/p/t1_*.bzl.

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

bool hasErrorContainingField(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("field `") != std::string::npos)
            return true;
    return false;
}

std::string nestedProgram(const std::string& name, const std::string& kw) {
    return "module M;\n"
           "public class Outer\n"
           "  public " + kw + " " + name + "\n"
           "  end " + name + ";\n"
           "end Outer;\n"
           "public class P\n"
           "  public static method Main(): int Console.WriteLine(\"ran\"); "
           "return 0; end Main;\n"
           "end P;\n";
}

}  // namespace

// §12.2 (v0.340, #6765): the issue's repro.
BEZEL_TEST(nested_type_object_trio_hiding_6765,
           nested_class_named_tostring_says_nested_type_spec_12_2) {
    auto r = runAot(nestedProgram("ToString", "class"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_FALSE(hasErrorContainingField(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(nested_type_object_trio_hiding_6765,
           nested_class_named_gethashcode_says_nested_type_spec_12_2) {
    auto r = runAot(nestedProgram("GetHashCode", "class"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `GetHashCode` hides the inherited "
           "`Object.GetHashCode`"));
    BEZEL_EXPECT_FALSE(hasErrorContainingField(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(nested_type_object_trio_hiding_6765,
           nested_class_named_equals_says_nested_type_spec_12_2) {
    auto r = runAot(nestedProgram("Equals", "class"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `Equals` hides the inherited `Object.Equals`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The other nested declaration kinds read the same way.
BEZEL_TEST(nested_type_object_trio_hiding_6765,
           nested_interface_named_tostring_says_nested_type_spec_12_2) {
    auto r = runAot(nestedProgram("ToString", "interface"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A nested type under a class with a user base reaches the same check.
BEZEL_TEST(nested_type_object_trio_hiding_6765,
           nested_class_in_derived_class_says_nested_type_spec_12_2) {
    auto r = runAot(R"(
module M;
public class B
end B;
public class D: B
  public class GetHashCode
  end GetHashCode;
end D;
public class P
  public static method Main(): int Console.WriteLine("ran"); return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `GetHashCode` hides the inherited "
           "`Object.GetHashCode`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Controls: a field and a property keep their own kind words.
BEZEL_TEST(nested_type_object_trio_hiding_6765,
           field_and_property_keep_their_kind_words_spec_12_2) {
    auto f = runAot(R"(
module M;
public class C
  public ToString: int := 0;
end C;
public class P
  public static method Main(): int return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        f, "field `ToString` hides the inherited `Object.ToString`"));
    auto p = runAot(R"(
module M;
public class C
  public property ToString: int => 1;
end C;
public class P
  public static method Main(): int return 0; end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        p, "property `ToString` hides the inherited `Object.ToString`"));
}

// Control: a nested type with any other name is accepted and the program runs.
BEZEL_TEST(nested_type_object_trio_hiding_6765,
           nested_class_with_other_name_accepted_spec_12_2) {
    auto r = runAot(nestedProgram("Inner", "class"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}
