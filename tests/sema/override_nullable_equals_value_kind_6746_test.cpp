// #6746 (§9.2, §10.5, §35.2, v0.340): on a value kind `override` is admitted
// only at the named Object-virtual signatures -- `Equals` taking the kind's own
// (non-nullable) type -- and "is rejected at the declaration on any other
// signature of those names". `override Equals(other: R?)` was admitted. Reject
// rows pin the declaration-site diagnostic; control rows pin that a PLAIN
// `Equals(other: R?)` is an ordinary member (§9.2) and that the own-typed
// override still compiles and runs.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}
const char* kMain = R"(
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)";
}  // namespace

BEZEL_TEST(override_nullable_equals_value_kind_6746, record_override_nullable_rejected_spec_9_2) {
    auto r = runAot(std::string(R"(
module M;
public record R(X: int)
  public override method Equals(other: R?): bool => true;
  public override method GetHashCode(): int => X;
end R;
)") + kMain);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "may not be declared `override` at this signature"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(override_nullable_equals_value_kind_6746, enum_override_nullable_rejected_spec_10_5) {
    auto r = runAot(std::string(R"(
module M;
public enum E
  A, B;
  public override method Equals(other: E?): bool => true;
  public override method GetHashCode(): int => 1;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "may not be virtual, abstract, or override"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(override_nullable_equals_value_kind_6746, choice_override_nullable_rejected_spec_35_2) {
    auto r = runAot(std::string(R"(
module M;
choice S A(x: int), B;
  public override method Equals(o: S?): bool => true;
  public override method GetHashCode(): int => 0;
end S;
)") + kMain);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "may not be virtual, abstract, or override"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: a plain `Equals(other: R?)` is an ordinary member (§9.2).
BEZEL_TEST(override_nullable_equals_value_kind_6746, record_plain_nullable_equals_accepted_spec_9_2_control) {
    auto r = runAot(std::string(R"(
module M;
public record R(X: int)
  public method Equals(other: R?): bool => true;
  public method GetHashCode(): int => X;
end R;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

BEZEL_TEST(override_nullable_equals_value_kind_6746, enum_plain_nullable_equals_accepted_spec_10_5_control) {
    auto r = runAot(std::string(R"(
module M;
public enum E
  A, B;
  public method Equals(other: E?): bool => true;
  public method GetHashCode(): int => 1;
end E;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}

// Control: the own-typed override stays admitted.
BEZEL_TEST(override_nullable_equals_value_kind_6746, record_override_own_typed_accepted_spec_9_2_control) {
    auto r = runAot(std::string(R"(
module M;
public record R(X: int)
  public override method Equals(other: R): bool => X = other.X;
  public override method GetHashCode(): int => X;
end R;
)") + kMain);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran\n"));
}
