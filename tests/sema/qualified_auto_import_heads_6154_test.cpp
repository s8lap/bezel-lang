// #6154 (§2.3, v0.338; §2.7 rule 2): the qualified spelling of an always-available interface head whose home module is an auto-imported module (`System.Collections.Generic.IEnumerable<T>`, `System.Collections.Linq.IOrderedEnumerable<T>`, `System.IComparable<T>`) was rejected as "unresolved type name `System`", because the built-in home-module table had a row only for `IGrouping` (#6080). Each such name must resolve by its module's qualified name, in parameter, return, local and generic-argument position, with and without `?`, while a wrong module qualifier and an unknown name keep their diagnostics.
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

// The issue shape: a qualified `System.Collections.Generic.IEnumerable<int>?` parameter, with no import, passed a null.
BEZEL_TEST(qualified_auto_import_heads_6154, generic_interface_parameter_issue_repro_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Generic.IEnumerable<int>?): int => 1;
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

// The non-nullable spelling of the same head, as a parameter type (no call needed to pin acceptance).
BEZEL_TEST(qualified_auto_import_heads_6154, generic_interface_parameter_non_nullable_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Generic.IEnumerable<int>): int => 1;
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// `IOrderedEnumerable<T>` is the other named declaration of the auto-imported `System.Collections.Linq` module (§2.3 Row 11), spelled with and without `?`.
BEZEL_TEST(qualified_auto_import_heads_6154, ordered_enumerable_parameter_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Linq.IOrderedEnumerable<int>?): int => 1;
  public static method Take2(g: System.Collections.Linq.IOrderedEnumerable<int>): int => 2;
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
BEZEL_TEST(qualified_auto_import_heads_6154, local_annotation_spec_2_3) {
    auto r = runAot(R"(
module M;

public class Program
  public static method Main(): int
    var e: System.Collections.Generic.IEnumerable<int>? := null;
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

// The qualified name as a return type.
BEZEL_TEST(qualified_auto_import_heads_6154, return_type_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Make(): System.Collections.Generic.IEnumerable<int>? => null;
  public static method Main(): int
    var e := C.Make();
    Console.WriteLine("ok");
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// The qualified interface as a generic argument of a qualified collection type (§2.3 Row 10 names both heads).
BEZEL_TEST(qualified_auto_import_heads_6154, generic_argument_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Generic.IReadOnlyList<System.Collections.Generic.IEnumerable<int>?>?): int => 1;
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

// `System.IComparable<T>` is a `System` (§2.3 Row 2) interface head, so the root-module spelling takes the same path.
BEZEL_TEST(qualified_auto_import_heads_6154, system_root_interface_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.IComparable<int>?): int => 1;
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

// A qualified name naming no always-available entity keeps its diagnostic: §2.3 names the module, not any qualifier.
BEZEL_TEST(qualified_auto_import_heads_6154, unknown_name_in_module_still_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Generic.NoSuchThing<int>?): int => 1;
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

// `IEnumerable<T>` is declared in System.Collections.Generic, not System.Text: a wrong module qualifier stays an unresolved name (§2.3 Row 10).
BEZEL_TEST(qualified_auto_import_heads_6154, wrong_module_qualifier_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Text.IEnumerable<int>?): int => 1;
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

// `IOrderedEnumerable<T>` is declared in System.Collections.Linq, not System.Collections.Generic (§2.3 Row 11).
BEZEL_TEST(qualified_auto_import_heads_6154, linq_head_under_generic_module_rejected_spec_2_3) {
    auto r = runAot(R"(
module M;

public class C
  public static method Take(g: System.Collections.Generic.IOrderedEnumerable<int>?): int => 1;
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
