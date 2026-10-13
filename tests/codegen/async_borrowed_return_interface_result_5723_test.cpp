// Codegen / soundness - issue #5723 (narrow), spec §30.5 *No fourth kind*
// (v0.338): "A `return` in an `async` body ... whose return type is `Task<T>`
// for class or interface T, whose operand is a borrow and not a `return
// await` ... is a compile error at the return site". The ownership pass's
// `isAsyncPatternAClassResultType` (the gate of
// `checkAsyncPatternABorrowedIdentifierReturn` for async methods and of the
// async-lambda twin) asked `isNonTrivialDisposeClass`, which is class-only,
// so `Task<IShape>` returning a plain parameter was ACCEPTED:
// the task's teardown then disposed an object the caller owned. MEASURED on
// the pre-fix tree: `using var s := new Sq(); using var t := Id(s);
// var r := await t;` at `Task<IShape>` -> ASan heap-use-after-free; the same
// program at a destructor-bearing class `Task<Dq>` -> rejected.
//
// The trivial-dispose CLASS gap (a class with no destructor is still accepted)
// and the `Object` gap are #5704's scope and are not touched here: an Object
// operand may be a runtime-managed box (`var o: Object := 7; return o;`),
// which §28.3 item 5 says is no borrow (async_await_extra_kinds_2226).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kNoFourthKind =
    "may only `return` an OWNED expression on a non-`await` return path";

constexpr const char* kShapes =
    "module M; "
    "public interface IShape method Area(): int; end IShape; "
    "public class Sq: IShape public method Area(): int => 4; end Sq; ";

}  // namespace

BEZEL_TEST(async_borrowed_return_interface_result_5723,
          reject_async_method_returns_plain_param_task_iface_30_5) {
    auto r = runAot(std::string(kShapes) +
        "public class Program "
        "  public static async method Id(p: IShape): Task<IShape> "
        "    return p; end Id; "
        "  public static async method Main(): Task<int> "
        "    using var s := new Sq(); using var t := Id(s); "
        "    var r := await t; return r.Area(); end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoFourthKind));
}

BEZEL_TEST(async_borrowed_return_interface_result_5723,
          reject_async_lambda_returns_plain_param_task_iface_30_5) {
    auto r = runAot(std::string(kShapes) +
        "public class Program "
        "  public static method Main(): int "
        "    var f: (IShape) -> Task<IShape> := "
        "      async lambda(p: IShape) => do return p; end; "
        "    return 0; end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoFourthKind));
}

// Control: returning an OWNED value (kind 1) at the interface result stays
// admitted and clean - a fresh `new Sq()` and a named owned local.
BEZEL_TEST(async_borrowed_return_interface_result_5723,
          accept_async_method_returns_owned_task_iface_looped_30_5) {
    const std::string s = std::string(kShapes) +
        "public class Program "
        "  public static async method Fresh(): Task<IShape> "
        "    return new Sq(); end Fresh; "
        "  public static async method Named(): Task<IShape> "
        "    var q := new Sq(); return q; end Named; "
        "  public static async method Main(): Task<int> "
        "    var n: int := 0; var i: int := 0; "
        "    while i < 60 do "
        "      do "
        "        using var a := Fresh(); var x := await a; "
        "        using var b := Named(); var y := await b; "
        "        n := n + x.Area() + y.Area() - 4; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = runAotChecked(s);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("240\n"));
}
