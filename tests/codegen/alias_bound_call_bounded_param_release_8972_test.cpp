// fix #8972 (§28.14 / §28.9, v0.359): a closure LOCAL passed to a METHOD
// whose body first BOUNDS its call-bounded formal to a LOCAL and then FORWARDS
// that local to another call-bounded parameter was never released: one closure
// cell + its env + its capture cell per call (MEASURED on the base: 15200
// byte(s) in 600 allocation(s) over 200 iterations). The direct spelling
// (`C.Id(k)`) releases it.
//
// WHY THE ALIASING FORWARD LEAKED. The closure local's discharge is its own
// SCOPE-END release, armed when the closure escape walk decides the local
// does not escape. A call ARGUMENT is not a read the callee merely looks at,
// so the walk classified the local as escaping — unless the argument position
// is a §28.14 CALL-BOUNDED parameter, where the callee provably holds no
// reference once the call returns. That relief proves the obligation with the
// CALLEE'S BODY (the fail-closed body walk), and the walk admitted a FORWARD
// of the FORMAL itself but not of a local bound from it: §28.14 (v0.359) admits
// "the callee may be invoked, BOUND TO LOCALS, and passed on as an argument to
// another call-bounded parameter", so `var h := g; … C.Id(h);` was refused by
// the walk, the relief declined, and the scope-end release was never armed.
//
// THE FIX. The body walk now admits a callee-local that is a PURE ALIAS of the
// formal — bound once from the formal, never reassigned, never stored,
// returned or captured — and forwards it at the same call-bounded position the
// formal's own forward would use (the same RECURSIVE proof: the forwarded
// position must be a plain function-typed parameter of a callee that holds
// nothing past ITS call). Every other use of the alias, and every alias
// spelling the walk does not recognise, still fails the walk closed, so a
// declined shape keeps leaking — a leak, never an early release.
//
// These are the suite's ASan boundary controls: a leak over a 200-iteration
// loop is the check a conformance-corpus case cannot express (the corpus
// decides an accept/reject verdict and a stdout value, not a leak count).
// Both rows here are RED on the base (the leak), so neither of them is a
// base-green control.

#include <string>

#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

namespace {

// Link+run `source` under ASan (LSan on). Returns 0 on a clean run matching
// `expectedExitCode`, 1 on any sanitizer report (leak, UAF, double-free) or
// a value mismatch, -1 if the toolchain is unavailable (SKIP).
int runUnderAsanValue(const std::string& source, int expectedExitCode,
                      const char* tag) {
    return bezel::test::runUnderAsanValue(source, expectedExitCode, tag,
                                          "detect_leaks=1:exitcode=99");
}

// Leak-checked. The `-1` (toolchain unavailable) case is NOT swallowed: the
// assertion runs on every other code, so the row cannot report a clean PASS
// with the sanitizer leg off.
void expectAsanClean(const std::string& source, int expectedExitCode) {
    const int a = runUnderAsanValue(source, expectedExitCode,
                                    "bezel_alias_bound_call_bounded_8972");
    if (a != -1) BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// The issue's own repro, verbatim: `F1` binds its call-bounded formal `g` to
// the local `h` and forwards `h` to `Id`'s call-bounded parameter, and `Id`
// only INVOKES it. Nothing in the chain holds the callback past its call —
// `h` is a local of `F1`'s own frame, which is gone when `F1` returns — so
// the caller's closure local `k` may be released at the caller's scope end.
//
// A fix that admitted the binding without admitting the FORWARD of the alias
// (leaving `h` a retention hazard) would leak here: this is the row that
// catches it.
BEZEL_TEST(alias_bound_call_bounded_param_release_8972,
           method_callee_binding_its_formal_to_a_local_and_forwarding_it_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Id(g: () -> int): int "
        "    return g(); "
        "  end Id; "
        "  public static method F1(g: () -> int): int "
        "    var h := g; "
        "    return C.Id(h); "
        "  end F1; "
        "  public static method Run(): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    return C.F1(k); "
        "  end Run; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      s := s + Run(); "
        "    end for; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;",
        0);
}

// The RECURSIVE arm: the alias is forwarded through TWO call-bounded levels —
// `F1` binds `g` to `h` and forwards `h` to `Mid`, which forwards its own
// formal to `Id`. A fix that admitted the alias's forward without recursing
// into the forwarded callee's own body (i.e. that proved only the FIRST hop)
// would leak here — this row is the one that catches it.
BEZEL_TEST(alias_bound_call_bounded_param_release_8972,
           alias_forwarded_through_two_call_bounded_levels_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Id(g: () -> int): int "
        "    return g(); "
        "  end Id; "
        "  public static method Mid(g: () -> int): int "
        "    return C.Id(g); "
        "  end Mid; "
        "  public static method F1(g: () -> int): int "
        "    var h := g; "
        "    return C.Mid(h); "
        "  end F1; "
        "  public static method Run(): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    return C.F1(k); "
        "  end Run; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      s := s + Run(); "
        "    end for; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;",
        0);
}
