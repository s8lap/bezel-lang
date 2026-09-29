// `a ?? b` is a K7 meet: an owned arm met with a borrowed arm is rejected in
// every position (issue #4964).
//
// Spec authority (v0.307), §28.3 producer table, the `a ?? b` row:
//   "The meet of the ownership states of `a` and `b` per K7 — the same
//   two-branch merge as the control-flow join above (K7.1–K7.3: owned ⊓ owned
//   = owned, exactly one arm's value produced and discharged; borrowed ⊓
//   borrowed = borrowed with the union of sources; mixed = rejected — equalize
//   the arms first). A named owned binding written as an arm is an unmarked
//   read and therefore a borrow arm (K3.13 …). … A `null`-literal or
//   bottom-typed (`throw`) arm absorbs to the other arm's state (K7.5)."
// K7.3 (§28.3 prose): "To compile a mixed merge, the program must equalize the
// branches before the merge; the compiler never equalizes for it".
//
// THE BUG. The ownership pass classified every built-in `??` with an owned
// right operand as an OWNED producer (#1029) and codegen transferred the left
// operand's value. MEASURED on e3b0bdf0 with `agent_check --probe --asan`:
//   * `p ?? new W()` over a NAMED owned local `p` (the issue): accepted as a
//     transfer the spec forbids (memory-safe on an `owned` return, a 48-byte
//     leak on a plain return);
//   * `g ?? new W()` over a BORROWED parameter `g`: accepted, then a
//     heap-use-after-free at a local `var`, a `using var`, an `owned` field
//     assignment, an `owned` return and an `owned` argument — the caller's
//     object is handed to a second owner.
//
// OBSERVABILITY. Pre-fix every REJECT row compiles clean, so each asserts the
// SPECIFIC diagnostic and `!r.ran`. The ACCEPT rows are the spec-legal
// neighbours (owned⊓owned, borrowed⊓borrowed, the K7.5 absorbing arms, a
// value-typed `??`) run under runAotChecked (generated-code ASan/LSan) with
// exact dispose counts; the owned⊓owned row is loop-amplified.
//
// per §28.3 (producer table `a ?? b` row; K7.1–K7.3, K7.5, K3.13), v0.307

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kNeedle =
    "the operands of this `??` mix an owned value with a borrowed arm";

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
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
const char* kPlainMain = "Console.WriteLine(h.F(true, g).Name()); ";
const char* kVoidMain = "h.F(true, g); ";

void expectRejected(const std::string& src) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// ===========================================================================
// REJECT — named owned local (a borrow arm, K3.13) met with a fresh owned arm.
// ===========================================================================

// The issue's program: `return p ?? new W();` under an `owned` return.
BEZEL_TEST(coalesce_mixed_meet_4964,
           named_owned_left_fresh_right_owned_return_rejected_4964_28_3) {
    expectRejected(program("owned Widget",
                           "var p: Widget? := new Widget(\"p\"); "
                           "return p ?? new Widget(\"n\");",
                           kOwnedMain));
}

// The same `??` as a local initializer (the issue's second spelling).
BEZEL_TEST(coalesce_mixed_meet_4964,
           named_owned_left_fresh_right_local_init_rejected_4964_28_3) {
    expectRejected(program("",
                           "var p: Widget? := new Widget(\"p\"); "
                           "var x := p ?? new Widget(\"n\"); "
                           "Console.WriteLine(x.Name());",
                           kVoidMain));
}

// Plain return: pre-fix this leaked both objects (measured 48 B / 2 objects).
BEZEL_TEST(coalesce_mixed_meet_4964,
           named_owned_left_fresh_right_plain_return_rejected_4964_28_3) {
    expectRejected(program("Widget",
                           "var p: Widget? := new Widget(\"p\"); "
                           "return p ?? new Widget(\"n\");",
                           kPlainMain));
}

// A chain: the inner `q ?? new W()` is itself the mixed meet.
BEZEL_TEST(coalesce_mixed_meet_4964,
           chain_inner_mixed_meet_rejected_4964_28_3) {
    expectRejected(program("owned Widget",
                           "var q: Widget? := new Widget(\"q\"); "
                           "return Widget.Make(false) ?? q ?? new Widget(\"n\");",
                           kOwnedMain));
}

// ===========================================================================
// REJECT — borrowed parameter met with a fresh owned arm. Each of these was
// a heap-use-after-free pre-fix (measured, e3b0bdf0).
// ===========================================================================

BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_fresh_right_owned_return_rejected_4964_28_3) {
    expectRejected(program("owned Widget", "return g ?? new Widget(\"n\");",
                           kOwnedMain));
}

BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_fresh_right_forgiven_owned_return_rejected_4964_17_8) {
    expectRejected(program("owned Widget",
                           "return (g ?? new Widget(\"n\"))!;", kOwnedMain));
}

BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_fresh_right_local_rejected_4964_28_3) {
    expectRejected(program("",
                           "var x := g ?? new Widget(\"n\"); "
                           "Console.WriteLine(x.Name());",
                           kVoidMain));
}

BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_fresh_right_using_var_rejected_4964_28_3) {
    expectRejected(program("",
                           "using var x := g ?? new Widget(\"n\"); "
                           "Console.WriteLine(x.Name());",
                           kVoidMain));
}

BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_fresh_right_owned_field_assign_rejected_4964_28_3) {
    expectRejected(
        program("", "_f := g ?? new Widget(\"n\");", kVoidMain));
}

BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_fresh_right_owned_argument_rejected_4964_28_3) {
    expectRejected(
        program("", "H.Take(g ?? new Widget(\"n\"));", kVoidMain));
}

// ===========================================================================
// ACCEPT — the legal meets, under generated-code ASan/LSan.
// ===========================================================================

// owned ⊓ owned = owned (K7.1): an `owned Widget?` call on the left, a fresh
// producer on the right. 100 calls on each path; each selected value is
// disposed exactly once by the caller, the other is never created.
BEZEL_TEST(coalesce_mixed_meet_4964,
           owned_call_left_fresh_right_loop_disposes_once_4964_k7_1) {
    JitResult r = runAotChecked(program(
        "owned Widget", "return Widget.Make(c) ?? new Widget(\"n\");",
        "for var i in 0..<100 do "
        "  using var a := h.F(true, g); using var b := h.F(false, g); "
        "end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose m\n"), 100);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose n\n"), 100);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose g\n"), 1);
}

// borrowed ⊓ borrowed = borrowed (K7.2): a parameter and a `using var`, used
// locally — nothing is disposed by the `??` result.
BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_local_right_borrow_meet_accepted_4964_k7_2) {
    JitResult r = runAotChecked(program(
        "", "using var d := new Widget(\"d\"); var x := g ?? d; "
            "Console.WriteLine(x.Name());",
        kVoidMain));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "g\ndispose d\nend\ndispose g\n");
}

// A `throw` arm absorbs (K7.5): `g ?? throw …` is the borrow `g`.
BEZEL_TEST(coalesce_mixed_meet_4964,
           param_left_throw_right_absorbs_accepted_4964_k7_5) {
    JitResult r = runAotChecked(program(
        "", "var x := g ?? throw new InvalidOperationException(\"none\"); "
            "Console.WriteLine(x.Name());",
        kVoidMain));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "g\nend\ndispose g\n");
}

// A value-typed `??` carries no ownership and is never a meet.
BEZEL_TEST(coalesce_mixed_meet_4964,
           value_typed_coalesce_unaffected_4964_28_3) {
    JitResult r = runAotChecked(program(
        "", "var k: int? := null; if c then k := 4; end; "
            "Console.WriteLine((k ?? 9).ToString());",
        "h.F(true, g); h.F(false, g); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n9\nend\ndispose g\n");
}
