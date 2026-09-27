// `return y!;` of an OWNED plain local through an `owned` return is a
// spec-legal move-out and must transfer ownership (issue #4955).
//
// Spec authority (v0.307):
//   * §17.8: "The null-forgiving operator preserves ownership: if `x` has
//     ownership state *owned* …, `x!` is owned; if `x` is borrowed, `x!` is
//     borrowed with the same source. The operator is purely a type-system
//     assertion (with a runtime null check); it does not move, copy, or
//     borrow beyond what reading `x` already does."
//   * §28.3: returning an owned local through an `owned` return transfers it
//     to the caller; the local is then not auto-dropped (K8).
//
// THE BUG. The return-transfer paths (the ownership pass's bare-identifier
// discharge in `handleReturn`, and codegen's ReturnStmt exclusion of the
// returned local from its scope-end disposal) matched only a bare
// `IdentifierExpr`. `y!` hid the identifier, so `y` was auto-dropped before
// `ret` and the caller then read and disposed the freed `Widget`. MEASURED on
// f321f94e and on the #4948 picks (`agent_check --probe --asan`): a clean
// compile, then `heap-use-after-free`.
//
// OBSERVABILITY. Each row is an ACCEPT row (the program is legal — it must
// NOT be rejected) asserting an error-free compile, `ran`, a zero exit and the
// EXACT stdout, including one `dispose y` line per `Widget` (the dispose
// count). The plain leg sees a double dispose / garbage read pre-fix; the
// generated-code ASan leg (`BEZEL_AOT_FORCE_SANITIZE=address`) sees the UAF.
// The loop row amplifies the disposal claim to exactly 100 disposals.
//
// per §17.8, §28.3, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int errorCount(const JitResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

void expectAccepted(const std::string& program, const std::string& out) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_EQ(errorCount(r), 0);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The issue's repro: `F`'s body is BODY; `Main` runs MAIN.
std::string program(const std::string& body, const std::string& main) {
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
               "  public method F(): owned Widget ") +
           body +
           "  end F; "
           "end H; "
           "public class Program "
           "  public static method Main(): int "
           "    using var h := new H(); " +
           main +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

const char* kReadOnce =
    "using var w := h.F(); Console.WriteLine(w.Name()); ";

}  // namespace

// The issue's exact program.
BEZEL_TEST(return_forgive_owned_local_4955,
           return_bang_owned_plain_local_transfers_4955_17_8) {
    expectAccepted(program("var y := new Widget(\"y\"); return y!;", kReadOnce),
                   "y\ndispose y\n");
}

// A nullable owned local: `y!` is the owned non-null value (§17.8).
BEZEL_TEST(return_forgive_owned_local_4955,
           return_bang_nullable_owned_local_transfers_4955_17_8) {
    expectAccepted(
        program("var y: Widget? := new Widget(\"y\"); return y!;", kReadOnce),
        "y\ndispose y\n");
}

// Repeated `!` preserves ownership at every layer.
BEZEL_TEST(return_forgive_owned_local_4955,
           return_bang_bang_owned_local_transfers_4955_17_8) {
    expectAccepted(
        program("var y: Widget? := new Widget(\"y\"); return y!!;", kReadOnce),
        "y\ndispose y\n");
}

// The return inside a `try` with a `catch` arm (the no-finally return edge
// with an active exception handler).
BEZEL_TEST(return_forgive_owned_local_4955,
           return_bang_owned_local_inside_try_transfers_4955_17_8) {
    expectAccepted(
        program("var y := new Widget(\"y\"); "
                "try return y!; "
                "catch Exception => Console.WriteLine(\"x\"), end try; "
                "return new Widget(\"e\"); ",
                kReadOnce),
        "y\ndispose y\n");
}

// Loop amplification of the disposal claim: every returned `Widget` is
// disposed EXACTLY once, by the caller's `using var` (100 lines, not 200 and
// not a crash).
BEZEL_TEST(return_forgive_owned_local_4955,
           return_bang_owned_local_loop_disposes_once_4955_17_8) {
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "dispose y\n";
    expected += "done\n";
    expectAccepted(
        program("var y := new Widget(\"y\"); return y!;",
                "for var i in 0..<100 do using var w := h.F(); end; "
                "Console.WriteLine(\"done\"); "),
        expected);
}
