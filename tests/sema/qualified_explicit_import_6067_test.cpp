// #6067 (§2.3 L161, v0.338; §2.7 rule 2): a built-in reached by the qualified name of a module this file imports (`System.Threading.Mutex` under `import System.Threading;`) is the plain spelling of that module's built-in. It was rejected as "unresolved type name `System`" although the import is present.
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

// #6067 issue shape: `System.Threading.Mutex` as a parameter type under `import System.Threading;`.
BEZEL_TEST(qualified_explicit_import_6067, mutex_qualified_with_import_accepted_spec_2_3) {
    auto r = runAot(R"(
module M;
import System.Threading;

public class B
  public constructor() end constructor;
  public method Take(m: System.Threading.Mutex): int => 1;
end B;
public class Program
  public static method Main(): int
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

// Control: the qualifier must be a module this file imports. `System.Threading.Mutex` under `import System.IO;` only is still unresolved (§2.3: `Mutex` needs `import System.Threading;`).
BEZEL_TEST(qualified_explicit_import_6067, mutex_qualified_wrong_import_still_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;
import System.IO;

public class B
  public constructor() end constructor;
  public method Take(m: System.Threading.Mutex): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `System`", 7, 25));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control (base behaviour kept): the alias spelling `T.Mutex` under `import System.Threading as T;`.
BEZEL_TEST(qualified_explicit_import_6067, mutex_alias_qualifier_accepted_spec_2_2) {
    auto r = runAot(R"(
module M;
import System.Threading as T;

public class B
  public constructor() end constructor;
  public method Take(m: T.Mutex): int => 1;
end B;
public class Program
  public static method Main(): int
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
