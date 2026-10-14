// #6079 (§2.3, v0.338 L161; §2.7 rule 2): the Task family is always-available, and its module-qualified spelling `System.Threading.Task` under `import System.Threading;` must resolve to the built-in. The qualified spelling was rejected as "unresolved type name `System`" because `Task` had no row in the builtin module-path table, so the #6040 qualified-builtin arm never matched it. Bare `Task` must keep resolving exactly as before.
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

// #6079 issue repro: `System.Threading.Task<int>` as a local annotation under `import System.Threading;`, awaited and printed.
BEZEL_TEST(qualified_task_6079, qualified_task_local_annotation_spec_2_3) {
    auto r = runAot(R"(
module M;
import System.Threading;

public class P
  public static async method Make(): Task<int>
    return 7;
  end Make;
  public static async method Main(): Task<int>
    var t: System.Threading.Task<int> := Make();
    var v := await t;
    Console.WriteLine($"{v}");
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// Control: the bare `Task<int>` spelling of the same program is unchanged by the fix (§2.3 Task family, always available with no import).
BEZEL_TEST(qualified_task_6079, bare_task_unchanged_spec_2_3) {
    auto r = runAot(R"(
module M;

public class P
  public static async method Make(): Task<int>
    return 7;
  end Make;
  public static async method Main(): Task<int>
    var t: Task<int> := Make();
    var v := await t;
    Console.WriteLine($"{v}");
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}

// The qualified spelling as a parameter type of a non-async method, with the bare head reached through the same qualifier-free name too.
BEZEL_TEST(qualified_task_6079, qualified_task_parameter_type_spec_2_3) {
    auto r = runAot(R"(
module M;
import System.Threading;

public class P
  public static method Take(t: System.Threading.Task): int => 1;
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// Control: the qualified name still needs its module imported (§2.3 names `System.Threading.Task` "under `import System.Threading;`"; §2.7 rule 2 needs the module in scope).
BEZEL_TEST(qualified_task_6079, qualified_task_without_import_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class P
  public static method Take(t: System.Threading.Task<int>): int => 1;
  public static method Main(): int
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `System`", 5, 32));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A wrong home module is not the Task family's module (§2.3 places `Task` in `System.Threading`, not `System.IO`).
BEZEL_TEST(qualified_task_6079, wrong_home_module_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;
import System.IO;

public class P
  public static method Take(t: System.IO.Task<int>): int => 1;
  public static method Main(): int
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
