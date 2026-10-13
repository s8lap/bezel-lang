// #5954 (§8.3, v0.338): the Equals/GetHashCode pairing applies at every
// nesting depth. A nested class that overrides `Equals` (or declares its own
// type's `Equals`) without `GetHashCode` is rejected exactly as a top-level
// class is. The pairing check previously walked module children only.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasPairingDiag(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("GetHashCode` (§8.3)") != std::string::npos &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §8.3 (v0.338, #5954): the issue's repro. A nested class overriding `Equals`
// with no `GetHashCode` is rejected at the nested `Equals`.
BEZEL_TEST(nested_class_equals_5954,
           nested_override_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class O
  public class N
    public override method Equals(a: Object?): bool => true;
  end N;
end O;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasPairingDiag(r, "a type that overrides `Equals`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5954): a nested class's own-typed `Equals` (plain spelling)
// fills the Object slot and obliges its own `GetHashCode` override.
BEZEL_TEST(nested_class_equals_5954,
           nested_own_typed_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class O
  public class N
    public constructor() end constructor;
    public method Equals(other: N): bool => true;
  end N;
end O;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasPairingDiag(r, "declaring `Equals(N)`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338, #5954): the obligation holds at depth two, not only one level
// into a nesting.
BEZEL_TEST(nested_class_equals_5954,
           doubly_nested_override_equals_without_gethashcode_rejected_spec_8_3) {
    auto r = runAot(R"(
module M;
class O
  public class P
    public class N
      public override method Equals(a: Object?): bool => true;
    end N;
  end P;
end O;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_TRUE(hasPairingDiag(r, "a type that overrides `Equals`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.3 (v0.338) control: a nested class that pairs its `Equals` with its own
// `override GetHashCode` is accepted and runs.
BEZEL_TEST(nested_class_equals_5954,
           nested_override_equals_with_gethashcode_accepted_spec_8_3) {
    auto r = runAot(R"(
module M;
class O
  public class N
    public override method Equals(a: Object?): bool => true;
    public override method GetHashCode(): int => 0;
  end N;
end O;
class Program public static method Main(): int return 0; end Main; end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
