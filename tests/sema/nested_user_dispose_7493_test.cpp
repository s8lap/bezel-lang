// Regression tests for issue #7493 — a NESTED `record` (or `class`) declaring
// a member named `Dispose` ESCAPED the §28.2 rule, because
// `Resolver::checkUserDeclaredDispose` walked `ctx_.module()->children` only.
//
// THE GOVERNING TEXT.
//
//   spec §28.2 (v0.350) — "Declaring a member named `Dispose` on a **class** or
//   a **record** is a **compile error** at that declaration, whatever its
//   parameter list; the diagnostic names the two customization points above —
//   the [§9.3] posture for a body member colliding with a compiler-generated
//   one, keyed on the name as [§12.2] is."
//
//   spec §28.2 (v0.350) — "**Interfaces are unaffected** — the rule reaches
//   **classes and records only**, and an interface may itself declare
//   `method Dispose();`: an interface member declares no body, so the name
//   displaces no compiler-generated member and traps no one."
//
//   spec §11.6 (v0.350) — "Nested types may themselves contain nested types",
//   so a nested type is an ordinary declaration of its kind at ANY depth, with
//   no exemption from §28.2.
//
// THE DEFECT.
//
//   The §28.2 rule carries NO depth qualifier, but the check walked only the
//   module's top-level types. A `class` or `record` nested inside another
//   type that declared `Dispose` was therefore admitted, while its module-level
//   twin was refused. The sibling check `checkRecordRestrictions` had the same
//   reach gap for §9.6/§9.7 and was fixed for #7490 by walking with
//   `forEachTypeAndNested` (NestedTypeWalk.hpp, §11.6); this lane applies the
//   same walk here.
//
// THE FIX.
//
//   A REACH change ONLY: `checkUserDeclaredDispose` walks the module's own
//   declaration tree with `forEachTypeAndNested`. The predicate (kind is class
//   or record, member named exactly `Dispose`), the diagnostic text and the
//   diagnostic site are untouched, so every module-level verdict is unchanged.
//
// THE ORACLE.
//
//   A rejection is pinned by the DIAGNOSTIC'S TEXT at a REAL LOCATION — never
//   by "the program did not compile" and never by an exit code.
//
// THE CONTROLS.
//
//   A nested INTERFACE declaring `method Dispose();` stays ADMITTED (§28.2:
//   "Interfaces are unaffected"); a nested class with a destructor and no
//   `Dispose` member stays ADMITTED; a member named `DisposeNow` (not the
//   exact name) stays ADMITTED (§12.2: the rule is keyed on the name); and a
//   nested record with no `Dispose` member stays ADMITTED.
//
// Cite: language spec §28.2 / §11.6 / §9.3 / §12.2 (v0.350).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
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

// ═══════════════════════════════════════════════════════════════════════════
// REJECT rows — the nested type's `Dispose` is now refused, at a REAL LOCATION.
// ═══════════════════════════════════════════════════════════════════════════

// The one-level shape: a record nested in a class. MEASURED on the pre-fix
// tip (build/bezelc rc=0, no diagnostic); the module-level twin is rc=1.
BEZEL_TEST(nested_user_dispose_7493,
           nested_record_declaring_dispose_is_rejected_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public record R",
        "    public method Dispose()",
        "    end Dispose;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 5,
        "record `R` may not declare a `Dispose` method directly (§28.2)"));
}

// The two-level shape inside a GENERIC outer class: a record nested in a
// class nested in a generic class. §11.6 — a nested type is an ordinary
// declaration at any depth. MEASURED on the pre-fix tip: admitted.
BEZEL_TEST(nested_user_dispose_7493,
           nested_record_two_levels_deep_in_generic_outer_is_rejected_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer<T>",
        "  public class Mid",
        "    public record R",
        "      public method Dispose()",
        "      end Dispose;",
        "    end R;",
        "  end Mid;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 5, 7,
        "record `R` may not declare a `Dispose` method directly (§28.2)"));
}

// The CLASS arm of the same rule, nested: "a **class** or a **record**".
// MEASURED on the pre-fix tip: admitted.
BEZEL_TEST(nested_user_dispose_7493,
           nested_class_declaring_dispose_is_rejected_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer<T>",
        "  public class C",
        "    public method Dispose()",
        "    end Dispose;",
        "  end C;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 5,
        "class `C` may not declare a `Dispose` method directly (§28.2)"));
}

// "whatever its parameter list": a nested record's optional-parameter
// `Dispose(mode: int := 0)` (§22.5 ambiguity) is refused too. MEASURED on the
// pre-fix tip: admitted.
BEZEL_TEST(nested_user_dispose_7493,
           nested_record_dispose_with_parameters_is_rejected_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public record R",
        "    public method Dispose(mode: int := 0)",
        "    end Dispose;",
        "  end R;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 5,
        "record `R` may not declare a `Dispose` method directly (§28.2)"));
}

// ═══════════════════════════════════════════════════════════════════════════
// ADMITTING TWINS — the rule does NOT reach these; they must keep compiling.
// ═══════════════════════════════════════════════════════════════════════════

// §28.2 "Interfaces are unaffected": a NESTED interface may declare
// `method Dispose();`, because an interface member declares no body. A fix
// that over-reaches to interfaces would refuse this.
BEZEL_TEST(nested_user_dispose_7493,
           nested_interface_declaring_dispose_is_still_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public interface IRelease",
        "    method Dispose();",
        "  end IRelease;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// A nested CLASS with a destructor and NO `Dispose` member is the sanctioned
// customization point (§28.2 "A `destructor` for cleanup logic"): admitted.
BEZEL_TEST(nested_user_dispose_7493,
           nested_class_with_a_destructor_and_no_dispose_is_still_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public class C",
        "    destructor",
        "    end destructor;",
        "  end C;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// §12.2 / §28.2: the rule is keyed on the exact name `Dispose`. A nested
// record's `DisposeNow` method is a different name and stays admitted.
BEZEL_TEST(nested_user_dispose_7493,
           nested_record_method_named_disposenow_is_still_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public record R",
        "    public method DisposeNow(): int => 1;",
        "  end R;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}

// Control: a nested record with no `Dispose` member at all is admitted.
BEZEL_TEST(nested_user_dispose_7493,
           nested_record_without_dispose_is_still_admitted_spec_28_2) {
    const std::string p = prog({
        "module M;",
        "public class Outer",
        "  public record R",
        "    public method Read(): int => 7;",
        "  end R;",
        "end Outer;",
        kMain,
        kMainBody,
        kMainPrint,
        kMainEnd,
        "end Program;",
    });
    expectCompiles(p);
}
