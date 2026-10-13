// #6040 (§2.3, v0.338; §2.7 rule 2): an always-available name is reached by its module's qualified name (`System.Exception`). The qualified spelling was rejected as "unresolved type name `System`" in every type position, and it must resolve with no import, while a wrong qualifier and a user declaration named `System` keep their diagnostics.
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

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// #6040 issue repro: `System.Exception` as an override parameter type; dispatches through the base-typed receiver.
BEZEL_TEST(qualified_system_exception_6040, parameter_type_issue_repro_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public virtual method Log(e: System.Exception): int => 1;
end B;
public class C: B
  public constructor() end constructor;
  public override method Log(e: System.Exception): int => 2;
end C;
public class Program
  public static method Main(): int
    var c: C := new C();
    var b: B := c;
    using var x := new System.Exception("x");
    Console.WriteLine(b.Log(x));
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// The qualified name in a local type annotation and in a `new` expression.
BEZEL_TEST(qualified_system_exception_6040, local_annotation_and_new_spec_2_3) {
    auto r = runAot(R"(
module M;

public class Program
  public static method Main(): int
    using var x: System.Exception := new System.Exception("x");
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

// The qualified name as an `owned` return type.
BEZEL_TEST(qualified_system_exception_6040, return_type_owned_spec_2_3) {
    auto r = runAot(R"(
module M;

public class Program
  public static method Make(): owned System.Exception => new System.Exception("x");
  public static method Main(): int
    using var x := Make();
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

// A wrong qualifier is not a module that exports the name: still an unresolved name (§2.3 names the module, not any qualifier).
BEZEL_TEST(qualified_system_exception_6040, unknown_qualifier_still_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public method Take(e: Foo.Exception): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `Foo`", 6, 25));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A near-miss of the module name is not the module (§2.3).
BEZEL_TEST(qualified_system_exception_6040, misspelled_system_still_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public method Take(e: Systm.Exception): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `Systm`", 6, 25));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Control: a qualified name that is NOT always-available still needs its import (§2.3: `System.Threading` requires `import System.Threading;` for `Mutex`).
BEZEL_TEST(qualified_system_exception_6040, non_always_available_needs_import_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public method Take(e: System.Threading.Mutex): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `System`", 6, 25));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A user declaration named `System` shadows the module prefix (§2.7 rule 1): `System.Exception` then names a member of the user type, not the built-in.
BEZEL_TEST(qualified_system_exception_6040, user_type_named_system_shadows_prefix_spec_2_7) {
    auto r = runAot(R"(
module M;

public class System
  public constructor() end constructor;
end System;
public class B
  public constructor() end constructor;
  public method Take(e: System.Exception): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "type `System` has no nested type named `Exception`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.3 L161 / §2.7 rule 2: the auto-imported `System.Text` needs no import; `StringBuilder` is always available.
BEZEL_TEST(qualified_system_exception_6040, auto_imported_text_module_accepted_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public method Take(s: System.Text.StringBuilder): int => 1;
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

// §2.3 auto-imported `System.Collections.Generic` (the whole of stdlib §4) needs no import.
BEZEL_TEST(qualified_system_exception_6040, auto_imported_generic_collections_accepted_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public method Take(d: System.Collections.Generic.Dictionary<string, int>): int => 1;
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

// §2.3 L161 / §2.7 rule 2: `System.Threading` is NOT auto-imported; its always-available `CancellationToken` needs `import System.Threading;` for the qualified spelling. Without the import: rejected as before.
BEZEL_TEST(qualified_system_exception_6040, cancellation_token_qualified_without_import_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class B
  public constructor() end constructor;
  public method Take(t: System.Threading.CancellationToken): int => 1;
end B;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorAt(r, "unresolved type name `System`", 6, 25));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.3 L161: with `import System.Threading;` the qualified spelling `System.Threading.CancellationToken` is the plain spelling of the imported module.
BEZEL_TEST(qualified_system_exception_6040, cancellation_token_qualified_with_import_accepted_spec_2_3) {
    auto r = runAot(R"(
module M;
import System.Threading;

public class B
  public constructor() end constructor;
  public method Take(t: System.Threading.CancellationToken): int => 1;
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

// Control (base behaviour, unchanged): an import alias qualifies the name by the alias (#4698 path), `T.CancellationToken` under `import System.Threading as T;`.
BEZEL_TEST(qualified_system_exception_6040, cancellation_token_alias_qualifier_accepted_spec_2_2) {
    auto r = runAot(R"(
module M;
import System.Threading as T;

public class B
  public constructor() end constructor;
  public method Take(t: T.CancellationToken): int => 1;
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
