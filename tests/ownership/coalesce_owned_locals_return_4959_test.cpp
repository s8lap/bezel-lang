// `return p ?? q;` over NAMED OWNED plain locals is a borrow of locals that
// are auto-dropped at scope end, so it is rejected (issue #4959).
//
// Spec authority (v0.307), §28.3 producer table, the `a ?? b` row:
//   "The **meet** of the ownership states of `a` and `b` per K7 … A **named
//   owned binding** written as an arm is an *unmarked read* and therefore a
//   **borrow** arm (K3.13; the transfer marker is rejected in `??`-operand
//   position — axiom A7): `a ?? b` over two named owned locals consumes
//   neither binding. To coalesce two named owned bindings into one owned
//   result, use the marked arm transfer —
//   `var r := (if a <> null then move a else move b);` (K3.4) — or equalize
//   by statement."
// K7.2: borrowed ⊓ borrowed = borrowed, "carrying the union of the arms'
// source sets". So `p ?? q` is a borrow sourced at {p, q}; both are owned
// plain locals auto-dropped at the method's exit (§28.3 K8), so returning the
// borrow lets it outlive its sources (§28.3 borrow-lifetime rule) — and under
// an `owned` return it is also a borrow returned as `owned` ("a borrowed
// local may not be returned from a method whose return type is `owned`").
// Either way the program is ILLEGAL.
//
// THE BUG. #4948's `??` fan-out deliberately left a bare owned-plain-local
// `??` operand unchecked, so the program compiled clean; both locals were
// auto-dropped before `ret` and the caller read (and disposed) a freed
// `Widget`. MEASURED (`agent_check --probe --asan`, lane r20-l33 on the #4948
// picks + #4955): heap-use-after-free.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN compile then a UAF, so each
// REJECT row asserts the SPECIFIC diagnostic AND `!r.ran`. The ACCEPT rows pin
// what stays legal: the spec's own remedy (the marked arm transfer and the
// statement equalization) and a local (non-escaping) use of the coalesce, each
// with exact stdout including the dispose lines; the loop row amplifies the
// remedy's disposal claim. (The `p ?? <fresh producer>` shape, once accepted
// here as the #1029 transfer, is a K7.3 mixed meet and rejected — #4964.)
//
// per §28.3 (producer table `a ?? b` row, K7.2, K3.13, K3.4), §17.2, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int errorCount(const JitResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

void expectRejected(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, needle));
    BEZEL_EXPECT_FALSE(r.ran);
}

void expectAccepted(const std::string& program, const std::string& out) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// `F`'s return type is RET and its body BODY; `Main` runs MAIN.
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
               "end Widget; "
               "public class H "
               "  public constructor() end constructor; "
               "  public method F(g: Widget): ") +
           ret + " " + body +
           "  end F; "
           "end H; "
           "public class Program "
           "  public static method Main(): int "
           "    using var h := new H(); "
           "    using var g := new Widget(\"g\"); " +
           main +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

const char* kOwnedMain =
    "using var w := h.F(g); Console.WriteLine(w.Name()); ";
const char* kBorrowMain = "Console.WriteLine(h.F(g).Name()); ";

const char* kPQ =
    "var p: Widget? := new Widget(\"p\"); var q := new Widget(\"q\"); ";

std::string needle(const std::string& name) {
    return "`??` operand `" + name +
           "` is a named owned binding, so it is a borrow arm";
}

}  // namespace

// ===========================================================================
// REJECT
// ===========================================================================

// The issue's program.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_coalesce_two_owned_locals_owned_return_rejected_4959_28_3) {
    expectRejected(program("owned Widget", std::string(kPQ) + "return p ?? q;",
                           kOwnedMain),
                   needle("p"));
}

// The null-left twin: the right operand is selected at runtime.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_coalesce_null_left_owned_return_rejected_4959_28_3) {
    expectRejected(
        program("owned Widget",
                "var p: Widget? := null; var q := new Widget(\"q\"); "
                "return p ?? q;",
                kOwnedMain),
        needle("q"));
}

// A plain (borrow) return: the same escape of auto-dropped sources.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_coalesce_two_owned_locals_borrow_return_rejected_4959_28_3) {
    expectRejected(program("Widget", std::string(kPQ) + "return p ?? q;",
                           kBorrowMain),
                   needle("p"));
}

// `!` over the coalesce does not change the source set (§17.8).
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_forgive_coalesce_owned_locals_rejected_4959_28_3_17_8) {
    expectRejected(program("owned Widget",
                           std::string(kPQ) + "return (p ?? q)!;", kOwnedMain),
                   needle("p"));
}

// Right-associative chain `p ?? (q ?? r)`.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_coalesce_chain_owned_locals_rejected_4959_28_3) {
    expectRejected(
        program("owned Widget",
                "var p: Widget? := null; var q: Widget? := new Widget(\"q\"); "
                "var r := new Widget(\"r\"); return p ?? q ?? r;",
                kOwnedMain),
        needle("q"));
}

// A borrowed parameter on the left does not launder the owned right operand.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_coalesce_param_then_owned_local_rejected_4959_28_3) {
    expectRejected(program("Widget",
                           "var q := new Widget(\"q\"); return g ?? q;",
                           kBorrowMain),
                   needle("q"));
}

// ===========================================================================
// ACCEPT — the spec's remedies and the legal neighbours.
// ===========================================================================

// §28.3's statement equalization: return the owned local itself on each path.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_equalized_by_statement_accepted_4959_28_3) {
    expectAccepted(
        program("owned Widget",
                std::string(kPQ) +
                    "if p <> null then return p!; end; return q;",
                kOwnedMain),
        "dispose q\np\ndispose p\ndispose g\n");
}

// A non-escaping local use of the coalesce is a legal borrow.
BEZEL_TEST(coalesce_owned_locals_return_4959,
           local_coalesce_of_owned_locals_accepted_4959_28_3) {
    expectAccepted(
        program("owned Widget",
                std::string(kPQ) +
                    "var r := p ?? q; Console.WriteLine(r.Name()); "
                    "return new Widget(\"n\");",
                kOwnedMain),
        "p\ndispose q\ndispose p\nn\ndispose n\ndispose g\n");
}

// fix #4964 (behaviour flip): `p ?? new W()` over a NAMED owned local was the
// #1029 owned-transfer shape and was accepted here. §28.3's `a ?? b` row makes
// the unmarked named binding a BORROW arm met with an OWNED arm — a K7.3 mixed
// meet, rejected ("equalize the arms first").
BEZEL_TEST(coalesce_owned_locals_return_4959,
           return_coalesce_owned_local_fresh_right_rejected_4964_k7_3) {
    expectRejected(program("owned Widget",
                           "var p: Widget? := new Widget(\"p\"); "
                           "return p ?? new Widget(\"n\");",
                           kOwnedMain),
                   "the operands of this `??` mix an owned value with a "
                   "borrowed arm");
}

// §28.3's own remedy, the marked arm transfer (K3.4), loop-amplified: the
// selected local is transferred and the other auto-dropped, each EXACTLY once
// per call (200 dispose lines for 100 calls, no double dispose, no leak).
BEZEL_TEST(coalesce_owned_locals_return_4959,
           marked_arm_transfer_remedy_loop_disposes_once_4959_28_3_k3_4) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "dispose q\ndispose p\n";
    expected += "done\n";
    expectAccepted(
        program("owned Widget",
                std::string(kPQ) +
                    "return (if p <> null then move p! else move q);",
                "for var i in 0..<100 do using var w := h.F(g); end; "
                "Console.WriteLine(\"done\"); "),
        expected + "dispose g\n");
}
