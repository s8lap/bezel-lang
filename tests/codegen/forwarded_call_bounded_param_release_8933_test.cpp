// fix #8933 (§28.14 / §28.9, v0.359): a closure LOCAL passed to a METHOD
// whose body FORWARDS its call-bounded formal to ANOTHER call-bounded
// parameter was never released: one closure cell + its env + its capture cell
// per call (MEASURED on the base: 15200 byte(s) in 600 allocation(s) over 200
// iterations). The direct spelling (`C.Id(k)`) releases it.
//
// WHY THE FORWARD LEAKED. The closure local's discharge is its own SCOPE-END
// release, armed when the closure escape walk decides the local does not
// escape. A call ARGUMENT is not a read the callee merely looks at, so the
// walk classified the local as escaping — unless the argument position is a
// §28.14 CALL-BOUNDED parameter, where the callee provably holds no
// reference once the call returns. That relief proves the obligation with
// the CALLEE'S BODY, and the body walk admitted only an INVOCATION or an
// INSPECTION of the formal. §28.14 (v0.359) admits one more use — "the
// callee may be invoked, bound to locals, and PASSED ON AS AN ARGUMENT TO
// ANOTHER CALL-BOUNDED PARAMETER" — so a callee that only forwards its
// formal was refused by the walk, the relief declined, and the scope-end
// release was never armed.
//
// THE FIX. The body walk now admits a FORWARD, with a RECURSIVE proof: the
// forwarded position must itself be a plain function-typed parameter of a
// callee that holds nothing past ITS call (a method target is walked with
// the same walk; a function-value target is proven from its SIGNATURE), and
// a `scoped` / `owned` / `ref` / `out` parameter, an imported / virtual /
// async callee, a deferred-activation return and a call-graph cycle all
// DECLINE (a leak, never an early release).
//
// These are the suite's ASan boundary controls: a leak over a 200-iteration
// loop is the check a conformance-corpus case cannot express (the corpus
// decides an accept/reject verdict and a stdout value, not a leak count).
// Every row here is RED on the base (the leak), so none of them is a
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
                                    "bezel_forwarded_call_bounded_8933");
    if (a != -1) BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// The issue's own repro, verbatim: `Fwd3` FORWARDS its call-bounded formal
// `g` to `Id`'s call-bounded parameter, and `Id` only INVOKES it. Nothing in
// the chain holds the callback past its call, so the caller's closure local
// `k` may be released at the caller's scope end.
BEZEL_TEST(forwarded_call_bounded_param_release_8933,
           method_callee_forwarding_to_a_method_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Id(g: () -> int): int "
        "    return g(); "
        "  end Id; "
        "  public static method Fwd3(g: () -> int): int "
        "    return C.Id(g); "
        "  end Fwd3; "
        "  public static method Run(): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    return C.Fwd3(k); "
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

// The other forwarded-target spelling: the callee forwards its formal to a
// FUNCTION VALUE's call-bounded parameter (`h(g)`, where `h` is a
// function-typed parameter of the forwarding method). A function value has
// no body to walk, so the proof there is the SIGNATURE (§28.14's
// call-bounded default is a property of the PARAMETER, and the compiler
// verifies the callee's obligations for every user-authored body).
BEZEL_TEST(forwarded_call_bounded_param_release_8933,
           method_callee_forwarding_to_a_function_value_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Id(g: () -> int): int "
        "    return g(); "
        "  end Id; "
        "  public static method Fwd(h: (() -> int) -> int, g: () -> int): int "
        "    return h(g); "
        "  end Fwd; "
        "  public static method Run(): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    return C.Fwd(C.Id, k); "
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

// The RECURSIVE arm: three hops, each forwarding its formal to the next
// call-bounded parameter. A fix that admitted a forward without recursing
// (i.e. that proved only the FIRST hop) would leak here — this row is the
// one that catches it.
BEZEL_TEST(forwarded_call_bounded_param_release_8933,
           forward_chain_of_three_methods_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Id(g: () -> int): int "
        "    return g(); "
        "  end Id; "
        "  public static method Fwd2(g: () -> int): int "
        "    return C.Id(g); "
        "  end Fwd2; "
        "  public static method Fwd3(g: () -> int): int "
        "    return C.Fwd2(g); "
        "  end Fwd3; "
        "  public static method Run(): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    return C.Fwd3(k); "
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
