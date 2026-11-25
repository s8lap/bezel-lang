// Ownership — issue #9206: a RECORD implementing an interface and carrying a
// FUNCTION-TYPED property, boxed into an interface-typed local
// (`var ih: IHx := r;`) and read through that interface (`ih.P(ih)`), leaked
// the 24-byte §8.6 interface box — one per boxing, unbounded.
//
// THE DEFECT. The box the §8.6 conversion minted is the local's to reclaim
// (§28.9: a box is runtime-managed; its reference must be dropped exactly
// once). The codegen escape walk (`BoxEscapeWalker`) decides whether the
// local may drop that reference at its scope end, and it classified the
// interface local as an ESCAPE — because the local is passed as a CALL
// ARGUMENT and the walk had no predicate for a callee whose body it cannot
// read. An escape keeps the local out of `boxReleaseEligible_`, so no
// scope-end `bz_box_release` was emitted and NOBODY ever released the box.
//
// MEASURED (r30-c1 @ 7fe91b2b7, `bezelc --sanitize=address`, 200 iterations):
//   "Direct leak of 4800 byte(s) in 200 object(s) allocated from: #1 in main"
// — 24 B per boxing, the §8.6 interface box.
//
// SPEC (v0.359).
//   §8.6: "When a value type is used where `Object` (or an interface
//     implemented by the value type) is expected, the value is **boxed**: a
//     heap-allocated wrapper holds the value."
//   §28.9: "Boxed value types — created implicitly when a value type is used
//     where `Object` (or an interface) is expected ... Boxed values are
//     runtime-managed"; a runtime-managed value's reference must be dropped
//     exactly once.
//   §28.1: a function-type parameter slot may carry `owned`, and "the modifier
//     is part of the function type's identity" — a PLAIN slot is a borrow.
//   §28.3.8 clause 3 (*The residual*): "The caller's check reads only the
//     callee's signature ... a borrow of a scope-bound local, passed to a
//     plain parameter that the callee stores ... is admitted at the call, for
//     a local, virtual, interface or imported callee alike. Neither source is
//     lifetime-extended: the temporary is disposed at its K8 boundary and the
//     local at its scope's end." The callee may STORE the borrow (the K4.3
//     trust clause); the CALLER's local is released at its scope's end all the
//     same, and nothing in that sentence turns on whether the callee's body is
//     in this module.
//
// THE ORACLES. `runAotChecked` compiles the program with the generated-code
// ASan pass and runs it, so a leak (the bug) and a double free / use-after-
// free (an over-release) both flip `r.ran`. Every row loops 200 times, so the
// 24 B per boxing accumulate to a report LSan cannot hide.
//
// THE FUNCTION VALUE'S SOURCE. The issue's own property creates a FRESH
// lambda per read, and a slot-dispatched read of a function-typed property
// leaks that closure too (issue #9175, a separate defect in the same
// program family, fixed on a separate branch). The rows below therefore take
// the function value from a `static readonly` field — the same interface, the
// same function-typed property, the same `ih.P(ih)` call — so this file pins
// the BOX leak alone. The static field draws one §22.12 rule 4 warning (a
// function-typed static is reachable from every domain); it is not the
// subject of any row here.
//
// The first row is the same mechanism without a static: the interface local is
// passed as an argument to a LOCAL function value.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own program, with the property's function value taken from a
// static field (see THE FUNCTION VALUE'S SOURCE above). 200 iterations, so
// the box leak is 200 * 24 B.
const char* kIssueShape =
    "module M;\n"
    "public interface IHx\n"
    "  readonly property P: (IHx) -> int;\n"
    "end IHx;\n"
    "public record Rx: IHx\n"
    "  public static readonly _f: (IHx) -> int := lambda(x: IHx) => 1;\n"
    "  public property P: (IHx) -> int => _f;\n"
    "  public constructor() end constructor;\n"
    "end Rx;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      var r := new Rx();\n"
    "      var ih: IHx := r;\n"
    "      t := t + ih.P(ih);\n"
    "    end for;\n"
    "    Console.WriteLine(t);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same mechanism with a LOCAL function value as the callee: the
// interface-typed local holding the box is passed as the argument.
const char* kLocalFunctionValue =
    "module M;\n"
    "public interface IHx\n"
    "  readonly property P: (IHx) -> int;\n"
    "end IHx;\n"
    "public record Rx: IHx\n"
    "  public property P: (IHx) -> int => lambda(x: IHx) => 1;\n"
    "  public constructor() end constructor;\n"
    "end Rx;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      var r := new Rx();\n"
    "      var ih: IHx := r;\n"
    "      var f: (IHx) -> int := lambda(x: IHx) => 1;\n"
    "      t := t + f(ih);\n"
    "    end for;\n"
    "    Console.WriteLine(t);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL — the issue's own control, sharpened: the same program with the
// property READ and CALLED, but with a CLASS receiver as the ARGUMENT, so the
// boxed local is NOT the argument. The box is freed today (nothing passes the
// local to a callee), and it must stay freed: a fix that also released the
// box where it is not passed — or released it twice — would double free it.
const char* kControlNoArgument =
    "module M;\n"
    "public interface IHx\n"
    "  readonly property P: (IHx) -> int;\n"
    "end IHx;\n"
    "public record Rx: IHx\n"
    "  public static readonly _f: (IHx) -> int := lambda(x: IHx) => 1;\n"
    "  public property P: (IHx) -> int => _f;\n"
    "  public constructor() end constructor;\n"
    "end Rx;\n"
    "public class Cx: IHx\n"
    "  public static readonly _f: (IHx) -> int := lambda(x: IHx) => 1;\n"
    "  public property P: (IHx) -> int => _f;\n"
    "  public constructor() end constructor;\n"
    "end Cx;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      var r := new Rx();\n"
    "      var ih: IHx := r;\n"
    "      var c := new Cx();\n"
    "      t := t + ih.P(c);\n"
    "    end for;\n"
    "    Console.WriteLine(t);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL — the class-receiver twin: a CLASS implementing the same interface
// is not boxed, so there is no box to release. A fix that released a class
// reference as if it were a box would corrupt the heap here.
const char* kControlClassReceiver =
    "module M;\n"
    "public interface IHx\n"
    "  readonly property P: (IHx) -> int;\n"
    "end IHx;\n"
    "public class Cx: IHx\n"
    "  public static readonly _f: (IHx) -> int := lambda(x: IHx) => 1;\n"
    "  public property P: (IHx) -> int => _f;\n"
    "  public constructor() end constructor;\n"
    "end Cx;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0 ..< 200 do\n"
    "      var c := new Cx();\n"
    "      t := t + c.P(c);\n"
    "    end for;\n"
    "    Console.WriteLine(t);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The strict oracle: no leak (the bug), no double free / use-after-free (an
// over-release), the printed total, and a zero exit code.
void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

// The issue's own shape: a record boxed into an interface-typed local, whose
// FUNCTION-typed property is read through that interface and called with the
// interface itself as the argument. The 24-byte box must be released at the
// local's scope end.
BEZEL_TEST(interface_box_release_function_value_arg_9206,
           spec_28_3_8_interface_box_passed_to_a_function_value_is_released) {
    expectClean(kIssueShape, "200\n");
}

// The same mechanism with a local function value as the callee.
BEZEL_TEST(interface_box_release_function_value_arg_9206,
           spec_28_3_8_interface_box_passed_to_a_local_function_value_is_released) {
    expectClean(kLocalFunctionValue, "200\n");
}

// CONTROL — the box is freed today where the local is not passed to a callee;
// it must stay freed (no double free).
BEZEL_TEST(interface_box_release_function_value_arg_9206,
           spec_28_3_8_control_the_box_is_released_where_it_is_not_passed) {
    expectClean(kControlNoArgument, "200\n");
}

// CONTROL — the class-receiver twin has no box at all.
BEZEL_TEST(interface_box_release_function_value_arg_9206,
           spec_28_9_control_the_class_receiver_twin_has_no_box) {
    expectClean(kControlClassReceiver, "200\n");
}
