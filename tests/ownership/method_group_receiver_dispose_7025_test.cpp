// fix #7025 (follow-up, spec §27.6 / §28.2, v0.343): the §28.2 USE-AFTER-
// DISPOSE warning for a METHOD GROUP's captured receiver.
//
// §27.6: "the instance is captured at formation — by reference for class- or
// interface-typed receivers". The capture is a BORROW of the receiver's block
// (§28.3), and the group's INVOCATION reads that borrow — the thunk dispatches
// the receiver's `Dispose`. So a receiver disposed before the invocation is
// exactly §28.2's use-after-dispose case: "the compiler must emit a
// compile-time warning when a local variable is used (read, borrowed from, or
// consumed) after an explicit `Dispose()` or direct `DisposeAsync()` call …
// on the same variable that dominates the use site on every path".
//
// The lambda twin's wording is followed ("a closure's captured borrow is read
// at invoke time, and this call reads a disposed value"), with the group in
// place of the closure — the two shapes carry the same obligation.
//
// The warning was UNPINNED at the review: it is emitted by the ownership pass
// (this oracle), while the runtime rows live in
// tests/codegen/class_dispose_method_group_7025_test.cpp.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.2 method-group warning, and the Note that names the disposal.
constexpr char kGroupDisposedWarn[] =
    "the method group over `d` was disposed before this invocation";
constexpr char kExplicitDisposeNote[] = "explicit Dispose() of `d` was here";

// A disposable class whose destructor is observable.
std::string program(const std::string& body) {
    return std::string("module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C "
        "  public static method Main(): int ") + body +
        " end Main; "
        "end C;";
}

bool hasWarning(const bezel::test::OwnershipResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool hasNote(const bezel::test::OwnershipResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Note &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The group is formed, the receiver is disposed, and the group is INVOKED:
// the invocation reads the captured borrow of a disposed value. §28.2's
// warning regime covers it — a warning, not an error.
BEZEL_TEST(method_group_receiver_dispose_7025, method_group_invocation_after_a_dispose_warns_28_2) {
    auto r = parseAndCheckOwnership(program(
        "var d := new D(); "
        "var f := d.Dispose; "
        "d.Dispose(); "
        "f(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasWarning(r, kGroupDisposedWarn));
    // The Note names the disposal site — §28.2's "on the same variable that
    // dominates the use site" is only actionable with it.
    BEZEL_EXPECT_TRUE(hasNote(r, kExplicitDisposeNote));
    // §28.2's regime for this shape is a WARNING (should-level), not an
    // error: the program is legal, and the runtime rows pin its behaviour.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The CONTROL: the same program WITHOUT the intervening dispose — the group
// reads a LIVE borrow, so §28.2's warning must NOT fire. Without this row
// the warning row could pass for a program that warns unconditionally.
BEZEL_TEST(method_group_receiver_dispose_7025, method_group_invocation_without_a_dispose_is_quiet_28_2) {
    auto r = parseAndCheckOwnership(program(
        "var d := new D(); "
        "var f := d.Dispose; "
        "f(); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(hasWarning(r, kGroupDisposedWarn));
}

// The §21.1 `const` spelling of the group binding — the same obligation (the
// borrow is a property of the CAPTURE, not of the declaration's
// mutability).
BEZEL_TEST(method_group_receiver_dispose_7025, const_method_group_invocation_after_a_dispose_warns_28_2) {
    auto r = parseAndCheckOwnership(program(
        "var d := new D(); "
        "const f := d.Dispose; "
        "d.Dispose(); "
        "f(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(hasWarning(r, kGroupDisposedWarn));
    BEZEL_EXPECT_TRUE(hasNote(r, kExplicitDisposeNote));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
