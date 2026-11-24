// fix #8953 (§28.14 / §28.9 / §27.5, v0.359): a closure LOCAL passed to a
// METHOD whose body CAPTURES its call-bounded (§28.14) formal in a closure
// that does NOT escape the callee was never released: one closure cell + its
// env + the captured local's capture cell per call (15200 byte(s) in 600
// allocation(s) over 200 iterations, MEASURED on the base r30-c1 1d2763251).
//
// WHY IT LEAKED. The discharge for a closure LOCAL is its own SCOPE-END
// release, armed when the closure escape walk decides the local does not
// escape. A call ARGUMENT is not a read the callee merely looks at, so the
// walk classified the local as escaping — unless the argument position is a
// §28.14 CALL-BOUNDED parameter, where the callee provably holds no
// reference once the call returns. That relief proves the obligation with the
// CALLEE'S BODY, and the body walk admitted only an INVOCATION, an
// INSPECTION, a FORWARD to another call-bounded parameter and a BINDING to a
// local. §28.14 (v0.359) admits one more use: the callback may be
// "captured by a closure that itself ESCAPES the callee" — a capture by a
// closure that does NOT escape is legal, and the callee then holds nothing
// past its own call (the capturing closure is a local of the callee,
// disposed at the callee's scope end). The walk declined every occurrence of
// the formal inside a lambda body — the #8482 rule, right for an ESCAPING
// closure and over-broad for one that is not, because the walk did not track
// whether the capturing closure escapes.
//
// THE FIX. The walk now admits a capture by a closure bound to a local of
// the callee's own body whose binding local does not ESCAPE the callee (the
// same fail-closed walk, run over the callee's body with the closure's local
// as its target: any use of that local other than a direct invocation of it —
// a store, a return, an argument, a capture by another closure — and any node
// kind the walk does not know, decline the admission). The callee's closure
// local is disposed at the callee's scope end, which is before the call
// returns, so the caller's closure local may be released at the caller's
// scope end.
//
// These are the suite's ASan boundary controls: a leak over a 200-iteration
// loop is the check a conformance-corpus case cannot express (the corpus
// decides an accept/reject verdict and a stdout value, not a leak count).
// Every row here is RED on the base (the leak), so none of them is a
// base-green control.
//
// The leak check is the EXIT-CODE assertion: `runAotChecked` compiles the
// program with the GENERATED-CODE AddressSanitizer (and LeakSanitizer) on
// and the harness's ASan exitcode is 99, so a leak makes the program exit
// 99 — `exitCode == 0` is the leak check, and the stdout assertion pins the
// program's own answer (the issue's repro prints 600).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The issue's own repro, verbatim: the callee `FwdL` CAPTURES its
// call-bounded formal `g` in a closure `h` of its own — a local of `FwdL`,
// disposed at `FwdL`'s scope end, so nothing in the callee outlives the
// call — and the caller's closure local `k` may be released at the caller's
// scope end. `Run()` returns 3 (the captured `m`), 200 times over: 600.
const char* kIssueRepro = R"BZL(
module M;
import System;
class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method FwdL(g: () -> int): int
    var h := lambda() => C.Id(g);
    return h();
  end FwdL;
  public static method Run(): int
    var m := 3;
    var k := lambda() => m;
    return C.FwdL(k);
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Run();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL";

// The other CAPTURE spelling: the capturing closure INVOKES the formal
// itself (`lambda() => g()`) instead of forwarding it to another
// call-bounded parameter. It is a different admission in the walk (the
// invocation, not the forward), so a fix that widened only the forward
// would leave this shape leaking — this row is the one that catches it.
const char* kCaptureByInvocation = R"BZL(
module M;
import System;
class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method FwdL(g: () -> int): int
    var h := lambda() => g();
    return h();
  end FwdL;
  public static method Run(): int
    var m := 3;
    var k := lambda() => m;
    return C.FwdL(k);
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Run();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
)BZL";

// Run the program under the generated-code AddressSanitizer (LeakSanitizer
// on) and pin BOTH its stdout and its exit code — the exit code is the leak
// check (a leak exits 99, the harness's ASan exitcode).
void expectCleanUnderAsan(const std::string& source,
                          const std::string& expectedStdout) {
    auto r = runAotChecked(source);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace

// The issue's own repro, verbatim.
BEZEL_TEST(captured_call_bounded_param_release_8953,
           issue_repro_verbatim_spec_28_14) {
    expectCleanUnderAsan(kIssueRepro, "600\n");
}

// The capture by INVOCATION (the other admission in the walk).
BEZEL_TEST(captured_call_bounded_param_release_8953,
           capture_by_invocation_spec_27_5) {
    expectCleanUnderAsan(kCaptureByInvocation, "600\n");
}
