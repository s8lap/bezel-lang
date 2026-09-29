// if/case mixed owned/borrowed meet passed as an `owned` argument is rejected
// at every read position, not only at a binding site, an expression body, or
// a return (issue #5060).
//
// Spec authority (v0.307), §28.3 producer table, the `(if c then v1 else v2)`
// row, and K7.3:
//   "An if-expression is the K7 meet of its then and else arms per the control-
//    flow join — the same two-branch merge as the CFG join above (K7.1–K7.3:
//    owned ⊓ owned = owned, exactly one arm's value produced and discharged;
//    borrowed ⊓ borrowed = borrowed with the union of sources; mixed =
//    rejected — equalize the arms first). A named owned binding written as an
//    arm is an unmarked read and therefore a borrow arm (K3.13 …). … A `null`-
//    literal or bottom-typed (`throw`) arm absorbs to the other arm's state
//    (K7.5)."
// K7.3 (§28.3 prose): "To compile a mixed merge, the program must equalize the
// branches before the merge; the compiler never equalizes for it".
//
// THE BUG. checkArmMeetMixed (Ownership.cpp) was driven only from a local-decl
// initializer, an expression body, and a return — so an if/case mixed meet
// reached anywhere else (most visibly an `owned` argument, but also a
// field-assignment RHS, an interpolation hole, a borrowed argument with its
// own obligations, etc.) silently accepted the meet and `producerState`
// classified the arm set via `conditionalCaseArmMeetIsOwned` for codegen —
// handing the borrow arm's caller-attributed owner a value the caller still
// owns. MEASURED on origin/main 0cbe41df with `agent_check --probe --asan`:
//   * `H.Take((if c then g else new Widget("n")))` over a BORROWED parameter
//     `g` and an `owned` parameter `w`: compiles clean, runs, disposes `g`
//     inside `Take`, then disposes `g` again in `Main`'s `using var g` —
//     heap-use-after-free.
// The `??` twin of this shape (`H.Take(g ?? new Widget("n"))`) was fixed on
// branch r23-c28 by the #4964 commit (239e0ca8). The if/case form needs the
// same treatment; widen the if/case driver beyond its current scope.
//
// OBSERVABILITY. Pre-fix every REJECT row compiles clean, so each asserts the
// SPECIFIC diagnostic and `!r.ran`. The ACCEPT rows are the spec-legal
// neighbours (all-borrowed, all-owned) and the `??` twin's non-regression.
//
// per §28.3 (producer table `(if c then v1 else v2)` row; K7.1–K7.3, K7.5,
// K3.13), v0.307

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kNeedle =
    "the arms of this if/case expression mix an owned value with a borrow";
const char* kCoalesceNeedle =
    "the operands of this `??` mix an owned value with a borrowed arm";

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// `H.F`'s return type is RET (empty: no return value) and its body BODY;
// `Main` runs MAIN.
std::string program(const std::string& ret, const std::string& body,
                    const std::string& main) {
    return std::string(
               "module M; "
               "public class Widget "
               "  private _n: string; "
               "  public constructor(n: string) _n := n; end constructor; "
               "  destructor Console.WriteLine($\"dispose {_n}\"); "
               "  end destructor; "
               "  public method Name(): string => _n; "
               "  public static method Make(f: bool): owned Widget? "
               "    if f then return new Widget(\"m\"); end; "
               "    return null; "
               "  end Make; "
               "end Widget; "
               "public class H "
               "  private owned _f: Widget? := null; "
               "  public constructor() end constructor; "
               "  public static method Take(owned w: Widget) "
               "    Console.WriteLine($\"take {w.Name()}\"); "
               "  end Take; "
               "  public method F(c: bool, g: Widget)") +
           (ret.empty() ? std::string(" ") : ": " + ret + " ") + body +
           "  end F; "
           "end H; "
           "public class Program "
               "  public static method Main(): int "
               "    using var h := new H(); "
               "    using var g := new Widget(\"g\"); " +
           main +
               "    Console.WriteLine(\"end\"); "
               "    return 0; "
               "  end Main; "
               "end Program;";
}

const char* kOwnedMain =
    "using var w := h.F(true, g); Console.WriteLine(w.Name()); ";
const char* kVoidMain = "h.F(true, g); h.F(false, g); ";

void expectRejected(const std::string& src) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// ===========================================================================
// REJECT — borrowed parameter met with a fresh owned arm in if/case positions
// that pre-fix never reached checkArmMeetMixed.
// ===========================================================================

// The issue's program: if/case mixed meet as an `owned` argument.
// Pre-fix: compiles clean and heap-use-after-frees under ASan; post-fix: §28.3
// K7.3 reject at the argument position.
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_param_left_fresh_right_owned_arg_rejected_5060_28_3) {
    expectRejected(program("", "H.Take((if c then g else new Widget(\"n\")));",
                           kVoidMain));
}

// If/case wrapped in a `!`: peel sees through the PostfixBangExpr to the
// ConditionalExpr (§17.8: `(if c then g else new W())!` has the meet's
// classification).
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_param_left_fresh_right_owned_arg_forgiven_rejected_5060_17_8) {
    expectRejected(program("",
                           "H.Take((if c then g else new Widget(\"n\"))!);",
                           kVoidMain));
}

// If/case as the RHS of an owned-field assignment.
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_param_left_fresh_right_owned_field_assign_rejected_5060_28_3) {
    expectRejected(program("", "_f := (if c then g else new Widget(\"n\"));",
                           kVoidMain));
}

// If/case wrapped in a member-access chain (`(if c then g else new W()).Name()`)
// — the new ConditionalExpr arm is reached via the MemberAccessExpr target
// recursion, mirroring #5071's `??` peel.
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_param_left_fresh_right_owned_member_rejected_5060_28_3) {
    expectRejected(program("",
                           "Console.WriteLine((if c then g else new Widget(\"n\")).Name());",
                           kVoidMain));
}

// ===========================================================================
// REJECT — non-regression: positions checkArmMeetMixed was ALREADY firing at
// (local initializer, expr body) must still reject.
// ===========================================================================

// If/case as a local initializer — the pre-existing handleLocalDecl driver.
// Pre-fix and post-fix: reject (the local-init driver already fired).
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_param_left_fresh_right_local_init_rejected_5060_28_3) {
    expectRejected(program("",
                           "var x := (if c then g else new Widget(\"n\")); "
                           "Console.WriteLine(x.Name());",
                           kVoidMain));
}

// If/case as an owned-return — the pre-existing return driver.
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_param_left_fresh_right_owned_return_rejected_5060_28_3) {
    expectRejected(program("owned Widget",
                           "return (if c then g else new Widget(\"n\"));",
                           kOwnedMain));
}

// ===========================================================================
// REJECT — non-regression: the `??` twin (`H.Take(g ?? new W())`) was
// already rejected by the #4964 commit and must stay rejected.
// ===========================================================================

// `??` as an `owned` argument: pin that #5060 did not regress #4964.
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           coalesce_param_left_fresh_right_owned_arg_rejected_4964_28_3) {
    JitResult r = runAot(program("", "H.Take(g ?? new Widget(\"n\"));", kVoidMain));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kCoalesceNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// ACCEPT — marker-free if/case with all-borrowed arms: the meet is borrowed
// and is a legal K7.2 join.
// ===========================================================================

// Both arms borrowed, no obligation, no meet-rejection. The result is a
// borrowed Widget; both sources survive. Uses H.F2 with two borrowed
// parameters (the K7.2 meet is borrowed).
BEZEL_TEST(if_case_arm_mixed_meet_owned_arg_5060,
           if_borrowed_meet_accepted_5060_k7_2) {
    const std::string src =
        "module M; "
        "public class Widget "
        "  private _n: string; "
        "  public constructor(n: string) _n := n; end constructor; "
        "  destructor Console.WriteLine($\"dispose {_n}\"); "
        "  end destructor; "
        "  public method Name(): string => _n; "
        "end Widget; "
        "public class H "
        "  public constructor() end constructor; "
        "  public method F2(c: bool, g: Widget, d: Widget) "
        "    var x := (if c then g else d); "
        "    Console.WriteLine(x.Name()); "
        "  end F2; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new H(); "
        "    using var g := new Widget(\"g\"); "
        "    using var d := new Widget(\"d\"); "
        "    h.F2(true, g, d); "
        "    h.F2(false, g, d); "
        "    Console.WriteLine(\"end\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}