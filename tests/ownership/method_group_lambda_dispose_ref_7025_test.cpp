// fix #7025 (follow-up, §27.6 / §28.2, v0.343): the §28.2 use-after-dispose
// WARNING for a closure whose body forms a method group over a captured
// receiver that was disposed.
//
// §27.6: "the instance is captured at formation — by reference for class- or
// interface-typed receivers". The capture is a BORROW of the receiver's block
// (§28.3), and a closure's captured borrow is READ at invoke time — so a
// receiver disposed before the invocation is §28.2's use-after-dispose case:
// "the compiler must emit a compile-time warning when a local variable is
// used (read, borrowed from, or consumed) after an explicit `Dispose()` or
// direct `DisposeAsync()` call … on the same variable that dominates the use
// site on every path".
//
// The warning is emitted by the ownership pass (this oracle); the runtime
// rows live in tests/codegen/method_group_lambda_dispose_ref_7025_test.cpp.
//
// The row pins the warning's PRESENCE — the compile-forward must not silence
// it — and the CONTROL pins that it does not fire without the intervening
// dispose.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// §28.2's closure wording.
constexpr char kClosureDisposedWarn[] =
    "closure captures `d` which was disposed before this invocation";

bool hasWarning(const bezel::test::OwnershipResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The closure is created, the receiver is disposed, and the closure is
// invoked: the invocation reads the captured borrow of a disposed value.
// §28.2's regime for this shape is a WARNING (the program is legal), and the
// warning must survive the compile-forward.
BEZEL_TEST(method_group_lambda_dispose_ref_7025,
           closure_capture_of_a_disposed_receiver_warns_28_2) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := lambda() => d.Dispose; "
        "  d.Dispose(); "
        "  f(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasWarning(r, kClosureDisposedWarn));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The CONTROL: the same closure WITHOUT the intervening dispose — the
// captured borrow is live, so §28.2's warning must NOT fire. Without this
// row the warning row could pass for a program that warns unconditionally.
BEZEL_TEST(method_group_lambda_dispose_ref_7025,
           closure_capture_of_a_live_receiver_is_quiet_28_2) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var f := lambda() => d.Dispose; "
        "  f(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasWarning(r, kClosureDisposedWarn));
}
