// #6080 (§2.3, v0.338; §2.7 rule 2): `System.Collections.Linq` is an auto-imported module whose only named declarations are `IGrouping<TKey, T>` and `IOrderedEnumerable<T>` (§2.3 Row 11). The qualified spelling `System.Collections.Linq.IGrouping<K, V>` was rejected as "unresolved type name `System`" even though the bare name resolves with no import, because the built-in home-module table had no row for the interface head `IGrouping`. It must resolve with no import, while a wrong module qualifier and a wrong qualifier root keep their diagnostics.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The aot driver reports the position in the message ("<path>:<line>:<col>: <msg>"); pin the needle AND the exact position.
bool hasErrorAt(const JitResult& r, const std::string& needle, unsigned line, unsigned col) {
    const std::string at = ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(at) != std::string::npos &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// #6080 issue shape: the qualified `System.Collections.Linq.IGrouping<int, int>` as a parameter type, with no import, passed a null grouping.
BEZEL_TEST(qualified_linq_igrouping_6080, parameter_type_issue_repro_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Linq.IGrouping<int, int>?): int => 1;
  public static method Main(): int
    Console.WriteLine(C.Take(null));
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// The qualified name in a local type annotation (no import, no `using`).
BEZEL_TEST(qualified_linq_igrouping_6080, local_annotation_spec_2_3) {
    auto r = runAot(R"(
module M;

public class Program
  public static method Main(): int
    var g: System.Collections.Linq.IGrouping<int, int>? := null;
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// Control: the bare `IGrouping<int, int>` (always available, §2.3 Row 11) keeps resolving unchanged.
BEZEL_TEST(qualified_linq_igrouping_6080, bare_name_control_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: IGrouping<int, int>?): int => 1;
  public static method Main(): int
    Console.WriteLine(C.Take(null));
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// `IGrouping` is declared in System.Collections.Linq, not System.Collections.Generic (§2.3 Row 11 / Row 10): the wrong module qualifier is still an unresolved name.
BEZEL_TEST(qualified_linq_igrouping_6080, wrong_module_qualifier_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Generic.IGrouping<int, int>?): int => 1;
  public static method Main(): int
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `System`", 5, 32));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A wrong qualifier root is not a module that exports the name (§2.3 names the module, not any qualifier).
BEZEL_TEST(qualified_linq_igrouping_6080, unknown_qualifier_still_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: Foo.IGrouping<int, int>?): int => 1;
  public static method Main(): int
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `Foo`", 5, 32));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
