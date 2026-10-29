// Regression tests for issue #7517 (owner ruling (b)) -- a member named
// `Dispose` declared on an ENUM or a CHOICE (value-like or owning) is a
// compile error, at module level and nested, with the same diagnostic as for
// a class or a record.
//
// THE GOVERNING TEXT.
//   spec §8.7 (v0.350) -- `Dispose()` is a member of a class type, of an
//   interface that is or extends `IDisposable` or declares `method Dispose();`
//   itself, "and of nothing else"; "enums, choices ... do not implement
//   `IDisposable`".
//   spec §28.2 (v0.350) -- the rule's rationale (the name promises a disposal
//   contract the kind does not have; on an owning choice it collides with the
//   compiler-generated disposal). "Interfaces are unaffected."
//
// THE ORACLE: the diagnostic TEXT at a REAL LOCATION. THE CONTROLS: an
// enum / choice with a differently named method and an interface declaring
// `method Dispose();` stay admitted.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Builds a program from `lines`; the 1-based line number of `lines[i]` is
// i + 1, so a row can name the line it expects the diagnostic on without
// counting newlines by hand.
std::string prog(const std::vector<std::string>& lines) {
    std::string s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += "\n";
    }
    return s;
}

// The oracle: the program was REJECTED AT COMPILE TIME, and the diagnostic
// carrying the expected TEXT sits at the expected REAL LOCATION.
bool rejectedAt(const std::string& program, int line, int col,
                const char* text) {
    JitResult r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string needle =
        ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            const std::size_t start = pos + needle.size();
            const std::size_t eol = d.message.find('\n', start);
            const std::string one =
                d.message.substr(start, eol == std::string::npos
                                          ? std::string::npos
                                          : eol - start);
            if (one.find(text) != std::string::npos) return true;
            pos += needle.size();
        }
    }
    return false;
}

// The control oracle for a program that must COMPILE and run.
void expectCompiles(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A `Program` with a `Main`, appended so an admitted program links and runs.
const char* kMain = "public class Program";
const char* kMainBody = "  public static method Main()";
const char* kMainPrint = "    Console.WriteLine(\"hi\");";
const char* kMainEnd = "  end Main;";

}  // namespace

BEZEL_TEST(dispose_on_enum_choice_7517, module_level_enum_declaring_dispose_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public enum E",
        "  Red, Green;",
        "  public method Dispose()",
        "  end Dispose;",
        "end E;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 3,
        "enum `E` may not declare a `Dispose` method directly (§28.2)"));
}

BEZEL_TEST(dispose_on_enum_choice_7517, nested_enum_declaring_dispose_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public enum E",
        "    Red, Green;",
        "    public method Dispose()",
        "    end Dispose;",
        "  end E;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 5, 5,
        "enum `E` may not declare a `Dispose` method directly (§28.2)"));
}

BEZEL_TEST(dispose_on_enum_choice_7517, module_level_value_choice_declaring_dispose_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public choice C",
        "  A(v: int), B;",
        "  public method Dispose()",
        "  end Dispose;",
        "end C;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 3,
        "choice `C` may not declare a `Dispose` method directly (§28.2)"));
}

BEZEL_TEST(dispose_on_enum_choice_7517, nested_value_choice_declaring_dispose_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public choice C",
        "    A(v: int), B;",
        "    public method Dispose()",
        "    end Dispose;",
        "  end C;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 5, 5,
        "choice `C` may not declare a `Dispose` method directly (§28.2)"));
}

BEZEL_TEST(dispose_on_enum_choice_7517, module_level_owning_choice_declaring_dispose_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Res",
        "end Res;",
        "public choice O",
        "  Held(r: Res), Empty;",
        "  public method Dispose()",
        "  end Dispose;",
        "end O;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 6, 3,
        "choice `O` may not declare a `Dispose` method directly (§28.2)"));
}

BEZEL_TEST(dispose_on_enum_choice_7517, nested_owning_choice_declaring_dispose_is_rejected_spec_8_7_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public class Res",
        "  end Res;",
        "  public choice O",
        "    Held(r: Res), Empty;",
        "    public method Dispose()",
        "    end Dispose;",
        "  end O;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 7, 5,
        "choice `O` may not declare a `Dispose` method directly (§28.2)"));
}

BEZEL_TEST(dispose_on_enum_choice_7517, enum_and_choice_with_other_method_names_are_admitted_spec_8_7) {
    const std::string p = prog({
        "module M;",
        "public enum E",
        "  Red, Green;",
        "  public method DisposeNow(): int => 1;",
        "end E;",
        "public choice C",
        "  A(v: int), B;",
        "  public method DisposeNow(): int => 2;",
        "end C;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

BEZEL_TEST(dispose_on_enum_choice_7517, interface_declaring_dispose_is_still_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public interface IRelease",
        "  method Dispose();",
        "end IRelease;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}
