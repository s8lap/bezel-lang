// #6796 (§12.2 Hiding is not allowed, §11.7 Static classes, §12.1, v0.340): the
// reject controls for the static-class exemption. §12.2: "a static member of the
// three names stays blocked (in a class that has instances — a `static class` is
// outside this rule, §11.7)". The exemption covers a static class's STATIC members
// at Object's own signature; the accept rows (declared, called, printed) are in
// tests/codegen/static_class_object_names_6796_test.cpp. These rows pin what the
// exemption does not reach: a non-static class's own static ToString(),
// GetHashCode() and Equals(other: Object?) stay blocked; a static class's instance
// ToString() stays rejected (§11.7's member rule, and §12.2's rule for a
// non-override instance member at Object's own signature); a static class's
// static override stays rejected (§12.1); a static class's own static
// ReferenceEquals stays blocked (§11.7: "is inherited by a static class as by every
// class, and §12.2's static-member clause blocks a static of that name there as
// anywhere").

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

// §12.2 (#6796 control): a non-static class's own static ToString(): string is
// still blocked at Object's own signature.
BEZEL_TEST(static_class_object_names_6796,
           non_static_class_static_tostring_still_rejected_spec_12_2) {
    auto r = runAot(R"BZL(
module M;
public class K
  public static method ToString(): string => "n01";
end K;
public class Program
  public static method Main(): int
    Console.WriteLine(K.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§12.2)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (#6796 control): the same for a non-static class's static GetHashCode().
BEZEL_TEST(static_class_object_names_6796,
           non_static_class_static_gethashcode_still_rejected_spec_12_2) {
    auto r = runAot(R"BZL(
module M;
public class K
  public static method GetHashCode(): int => 2;
end K;
public class Program
  public static method Main(): int
    Console.WriteLine(K.GetHashCode());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `GetHashCode` hides the inherited `Object.GetHashCode`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§12.2)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (#6796 control): the same for a non-static class's static
// Equals(other: Object?).
BEZEL_TEST(static_class_object_names_6796,
           non_static_class_static_equals_object_still_rejected_spec_12_2) {
    auto r = runAot(R"BZL(
module M;
public class K
  public static method Equals(other: Object?): bool => true;
end K;
public class Program
  public static method Main(): int
    Console.WriteLine(K.Equals(1));
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `Equals` hides the inherited `Object.Equals`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§12.2)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §11.7 (#6796 control): an INSTANCE ToString() in a static class is not exempted:
// the static-class member rule rejects it, and the Object-signature report stands.
BEZEL_TEST(static_class_object_names_6796,
           static_class_instance_tostring_still_rejected_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public method ToString(): string => "s14";
end V;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static class `V` may not declare instance"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§11.7"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `ToString` hides the inherited `Object.ToString`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.1 (#6796 control): a static class's static override is still rejected as a
// virtual-kind member; the exemption is for a plain static member only.
BEZEL_TEST(static_class_object_names_6796,
           static_class_static_override_tostring_still_rejected_spec_12_1) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static override method ToString(): string => "s15";
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "may not be virtual, abstract, or override (§12.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §11.7 (#6796 control): Object's static ReferenceEquals is inherited by a static
// class, and a static member of that name stays blocked there (§12.2). The
// exemption covers the three names only.
BEZEL_TEST(static_class_object_names_6796,
           static_class_static_referenceequals_still_rejected_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method ReferenceEquals(a: Object?, b: Object?): bool => true;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `ReferenceEquals` hides the inherited `Object.ReferenceEquals`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§11.7, §12.2)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
