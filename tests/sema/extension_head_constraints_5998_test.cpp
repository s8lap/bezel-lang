// fix #5998 part (1) / spec §14.2 *A type parameter as the type argument*
// (case 1, v0.338) and §26.4 (v0.338): a block type bound decides a `class`
// requirement of a generic extension head only when it guarantees class. A
// class bound, a bound naming IDisposable, or an interface extending IDisposable
// guarantees it (L2391). An ordinary interface bound does not, so
// `extension OwningArray<T> where T: IShape` is a compile error naming
// `where T: class`. (Item 2, the builtin Dictionary `hashable` key, is
// withdrawn: the key rule for Dictionary/Set is the type-level §3.6 rule 2.)
//
// Every reject row asserts the §14.2 needle naming the constraint and the
// parameter, and the head's real file:line:col (the location survives only as
// text in the packed message, see chain_position_field_read_3829_test.cpp).
// Every accept row asserts the program ran and printed the expected stdout.

#include <regex>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The §14.2 head diagnostic that names `needle`, or empty.
std::string headDiagnosticText(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(§14.2)") != std::string::npos)
            return d.message;
    return {};
}

bool hasHeadDiagnostic(const JitResult& r, const std::string& needle) {
    return !headDiagnosticText(r, needle).empty();
}

// True when the head diagnostic naming `needle` carries the real location
// `:line:col: ` of the extension head at `headText` in a one-line source.
bool headDiagnosticAt(const JitResult& r, const std::string& needle,
                      unsigned line, unsigned col) {
    const std::string msg = headDiagnosticText(r, needle);
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    std::smatch m;
    if (!std::regex_search(msg, m, kLoc)) return false;
    return std::stoul(m[1]) == line && std::stoul(m[2]) == col;
}

// The 1-based column of the first `head` in a one-line source.
unsigned columnOf(const std::string& src, const std::string& head) {
    return static_cast<unsigned>(src.find(head)) + 1;
}

const std::string kWidget =
    "module M; "
    "class Widget "
    "  public constructor() end constructor; "
    "end Widget; ";

}  // namespace

// ---------------------------------------------------------------------------
// Reject rows, part (1): a class requirement over an ordinary interface bound.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_extension_head_constraints_5998,
           ordinary_interface_bound_does_not_guarantee_class_rejected_sec_14_2) {
    // §14.2 line 2391: an ordinary interface's conforming set is open, so
    // `where T: IShape` does not guarantee `class` for OwningArray's element.
    const std::string src =
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "extension OwningArray<T> where T: IShape "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    // The strong claim: a real compile-time rejection with a located
    // diagnostic, not a runtime crash, signal, timeout or ASan report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `T` does not satisfy the constraint `where T: class`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(
        r, needle, 1, columnOf(src, "OwningArray<T> where T: IShape")));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           interface_bound_on_class_requirement_rejected_sec_14_2) {
    // §14.2 line 2391: a user class `Keep<T> where T: class` requires class of
    // its parameter; an extension restating it with an ordinary interface
    // bound does not state a class guarantee, so the head is rejected.
    const std::string src =
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Keep<T> where T: class "
        "  public constructor() end constructor; "
        "end Keep; "
        "extension Keep<T> where T: IShape "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(src);
    // The strong claim: a real compile-time rejection with a located
    // diagnostic, not a runtime crash, signal, timeout or ASan report.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `T` does not satisfy the constraint `where T: class`";
    BEZEL_EXPECT_TRUE(hasHeadDiagnostic(r, needle));
    BEZEL_EXPECT_TRUE(headDiagnosticAt(
        r, needle, 1, columnOf(src, "Keep<T> where T: IShape")));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           owning_array_class_restated_accepted_and_runs_sec_26_4) {
    // §26.4 (v0.338): `extension OwningArray<T> where T: class` restates the
    // stdlib requirement; the extension applies to OwningArray<Widget>.
    auto r = runAot(
        kWidget +
        "extension OwningArray<T> where T: class "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var a := new OwningArray<Widget>(); "
        "    Console.WriteLine(a.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           idisposable_type_bound_guarantees_class_accepted_sec_14_2) {
    // §14.2 line 2391: a bound naming IDisposable guarantees `class`, so the
    // head is admitted. The head is declared only; no receiver is instantiated.
    auto r = runAot(
        "module M; "
        "extension OwningArray<T> where T: IDisposable "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           interface_extending_idisposable_guarantees_class_accepted_sec_14_2) {
    // §14.2 line 2391: an interface extending IDisposable guarantees `class`.
    auto r = runAot(
        "module M; "
        "interface IDsp: IDisposable method Area(): int; end IDsp; "
        "extension OwningArray<T> where T: IDsp "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

BEZEL_TEST(sema_extension_head_constraints_5998,
           class_bound_guarantees_class_accepted_sec_14_2) {
    // §14.2 line 2391: a bound naming a class guarantees `class`.
    auto r = runAot(
        "module M; "
        "class Base end Base; "
        "extension OwningArray<T> where T: Base "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}


BEZEL_TEST(sema_extension_head_constraints_5998,
           class_bound_whose_base_list_names_required_interface_accepted_sec_14_2) {
    // §14.2 L2391 and rule 6 (L2428): Keep requires `where T: IS`. The block
    // bound `Circle` is a class whose own base list names IS, so every
    // Circle satisfies IS at every substitution the bound admits, and the
    // head is admitted. The program instantiates and runs.
    auto r = runAot(
        "module M; "
        "interface IS method A(): int; end IS; "
        "class Circle : IS "
        "  public constructor() end constructor; "
        "  public method A(): int return 1; end A; "
        "end Circle; "
        "class Keep<T> where T: IS "
        "  public constructor() end constructor; "
        "end Keep; "
        "extension Keep<T> where T: Circle "
        "  public method P(): int return 7; end P; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var k := new Keep<Circle>(); "
        "    Console.WriteLine(k.P().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
