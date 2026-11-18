// fix #8872 (§28.14 / §28.9 / §27.5, v0.359): a closure LOCAL passed as an
// ARGUMENT to a call made THROUGH A FUNCTION VALUE — a lambda-typed local
// `h(k)`, or a function-typed parameter `g(k)` — was never released. The
// direct-method-call spelling (`C.Ap(k)` with `Ap(g: () -> int)`) releases it.
//
// WHY THE INDIRECT SPELLING LEAKED. The closure local's discharge is its own
// SCOPE-END release, armed when the closure escape walk decides the local
// does not escape. A call ARGUMENT is not a read the callee merely looks at,
// so the walk classified the local as escaping — unless the argument position
// is a §28.14 CALL-BOUNDED parameter, where the callee provably holds no
// reference once the call returns. That relief (the escape walker's
// `closureArgBindsMethodOwnTypeParamBorrow` arm) proves the obligation with
// the CALLEE'S BODY, so it applies only to a method callee. A function-VALUE
// callee has no body to walk, and the argument stayed an escape: the
// scope-end release was never armed and one closure cell + its env leaked per
// call.
//
// MEASURED on the base (r30-c1 @ d8e779e5c, `bezelc --sanitize=address`,
// LSan, 200 iterations): the repro below leaked "15200 byte(s) in 600
// allocation(s)" — k's 64-byte cell, k's 8-byte env and the captured `m`'s
// 4-byte cell, every iteration. With the fix the same program is
// ASan-clean.
//
// These are the suite's ASan boundary controls: a leak over a 200-iteration
// loop is the check a conformance-corpus case cannot express (the corpus
// decides an accept/reject verdict and a stdout value, not a leak count).
// Two rows — the two callee spellings the issue names — and no more.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>

namespace {

namespace fs = std::filesystem;

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
// with the sanitizer leg off (the #3682 shape `scripts/
// check_native_asan_leg_skips.py` ratchets).
void expectAsanClean(const std::string& source, int expectedExitCode) {
    const int a = runUnderAsanValue(source, expectedExitCode,
                                    "bezel_indirect_call_8872");
    if (a != -1) BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// The issue's own repro: `k` is a closure local that CAPTURES `m` (so the
// leak is the closure cell, its env AND the capture cell), `h` is a
// lambda-typed local whose parameter `g` is a PLAIN function-typed parameter
// (§28.14's call-bounded default), and `h(k)` passes `k` at that
// call-bounded position.
BEZEL_TEST(indirect_call_closure_arg_release_8872,
           lambda_local_callee_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Run(): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    var h := lambda(g: () -> int) => g(); "
        "    return h(k); "
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

// The second callee spelling the issue names: the callee is a FUNCTION-TYPED
// PARAMETER of `Run`, not a lambda-typed local. Same obligation (§28.14's
// call-bounded default holds for a function value's own parameter), same
// discharge (the argument's scope-end release in the CALLER's frame).
BEZEL_TEST(indirect_call_closure_arg_release_8872,
           function_typed_parameter_callee_releases_the_argument) {
    expectAsanClean(
        "module M; "
        "import System; "
        "class C "
        "  public static method Run(h: (() -> int) -> int): int "
        "    var m := 3; "
        "    var k := lambda() => m; "
        "    return h(k); "
        "  end Run; "
        "  public static method Main(): int "
        "    var h := lambda(g: () -> int) => g(); "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      s := s + Run(h); "
        "    end for; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;",
        0);
}
