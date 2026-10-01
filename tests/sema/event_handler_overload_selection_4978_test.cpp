// Regression tests for #4978 (residual) - spec §27.6 "Method references to
// overloaded methods" (v0.313): a method GROUP at a plain event-handler
// position (stdlib §6.5: `(T) -> ()`) is resolved against the slot's target
// function type, so the overload that fits the payload is bound - by sema and
// by codegen alike - regardless of declaration order. Before the fix the
// handler check (checkEventHandlerArgument, #4968) tried only the
// first-declared overload and rejected `Subscribe(H.On)` when it did not fit.
//
// Not covered (still open under #4978): the §27.6 conversion ADAPTER - a
// handler taking the payload only through a representation-changing
// conversion stays rejected (plain_event_handler_signature_4968 row
// conversion_bridged_handler_rejected_until_4978_spec_27_6).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& groupDecls, const std::string& wire) {
    return "module M;\n"
           "class H\n" + groupDecls + "end H;\n"
           "class Btn\n"
           "  private owned readonly _src: EventSource<int> := new();\n"
           "  public property Face: IEvent<int> => _src;\n"
           "  public event Clicked: int;\n"
           "  public method Wire()\n    " + wire + "\n  end Wire;\n"
           "  public method Fire(n: int)\n"
           "    Clicked.Raise(n); _src.Raise(n);\n"
           "  end Fire;\n"
           "end Btn;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var b := new Btn();\n"
           "    b.Wire();\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char* kStrThenInt =
    "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On;\n"
    "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n";
const char* kIntThenStr =
    "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On;\n"
    "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On;\n";

}  // namespace

// The three plain routes (event member, IEvent face, EventSource) with the
// non-fitting overload declared FIRST.
BEZEL_TEST(event_handler_overload_selection_4978,
           str_first_group_binds_int_overload_on_three_routes_spec_27_6) {
    auto r = runAot(prog(kStrThenInt,
        "using var a := Clicked.Subscribe(H.On);\n"
        "    using var f := Face.Subscribe(H.On);\n"
        "    using var d := _src.Subscribe(H.On);\n"
        "    Clicked.Raise(7); _src.Raise(7);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("int:7\nint:7\nint:7\n"));
}

// Mirror: int declared first (declaration order must not matter).
BEZEL_TEST(event_handler_overload_selection_4978,
           int_first_group_binds_int_overload_spec_27_6) {
    auto r = runAot(prog(kIntThenStr,
        "using var d := _src.Subscribe(H.On); _src.Raise(9);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("int:9\n"));
}

// `+=` / `-=` desugar to Add / Remove (§17.2) and share the same check.
BEZEL_TEST(event_handler_overload_selection_4978,
           plus_equals_group_binds_int_overload_spec_27_6) {
    auto r = runAot(prog(kStrThenInt,
        "Clicked += H.On; Clicked.Raise(3); Clicked -= H.On; Clicked.Raise(4);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("int:3\n"));
}

// A group with NO overload accepting the payload is still rejected.
BEZEL_TEST(event_handler_overload_selection_4978,
           group_without_fitting_overload_still_rejected_spec_27_6) {
    auto r = runAot(prog(
        "  public static method On(x: string) Console.WriteLine(x); end On;\n"
        "  public static method On(x: bool) Console.WriteLine($\"{x}\"); end On;\n",
        "using var d := _src.Subscribe(H.On);"));
    BEZEL_EXPECT_FALSE(r.ran);
    // per §27.6 (v0.313): the handler-conversion reject, at a real location,
    // not a crash or timeout masquerading as `!ran` (#3892).
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "is not implicitly convertible to the target type"));
}
