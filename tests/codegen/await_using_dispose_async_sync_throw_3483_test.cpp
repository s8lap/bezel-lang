// Codegen — issue #3483 (§28.12, v0.350): a NON-`async` `DisposeAsync(): Task`
// that throws SYNCHRONOUSLY under `await using`.
//
//     public class N: IAsyncDisposable
//       public method DisposeAsync(): Task
//         K.da := K.da + 1;
//         throw new InvalidOperationException("boom");
//       end DisposeAsync;
//       ...
//     await using var d := Program.M0();   // M0(): owned IAsyncDisposable
//
// §28.12 declares the member without `async` ("`async` is an
// implementation-side modifier"), so an implementation may be a plain method
// that throws before it ever produces a task. Both `await using` arms — the
// static arm (`new N()` settles the class) and the run-time arm (an interface,
// `Object` or call head, the #3483 itab dispatch) — awaited the null that such
// a throw returns: SEGV at 0x10 in `bz_task_is_done`, from the enclosing
// method's Resume (MEASURED, review probes
// /var/tmp/bezel-scratch/r37-rev-l4b/p/t2h_{0_static,1_iad,2_id,3_obj}.bzl;
// the static head crashed on the r30-c1 base too, the three run-time heads
// crashed once the run-time arm reached them). A direct `await
// n.DisposeAsync()` was fine: the ordinary call path runs emitPendingCheck.
//
// §28.12 step 2, v0.350, verbatim: "This includes the fault edge: when
// `DisposeAsync` completes by throwing, logical disposal is over — the runtime
// runs the cascade and the chain, then the exception propagates from the
// await." So at every head: the throw is caught by the caller's `catch` (50 of
// 50), the destructor body does NOT run (step 1: `DisposeAsync` supersedes it,
// `dt=0`), and the owned `Cell` is cascaded (`cells=0`, ASan-clean under
// runAotChecked). Each row loops 50x.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Shared prelude: an owned `Cell` counts the cascade, `K.da` counts
// `DisposeAsync` entries, `K.dt` counts destructor bodies.
constexpr const char* kPrelude = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static da: int := 0;
  public static dt: int := 0;
  public constructor() end constructor;
end K;
public class N: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(): Task
    K.da := K.da + 1;
    throw new InvalidOperationException("boom");
  end DisposeAsync;
  destructor K.dt := K.dt + 1; end destructor;
end N;
)";

// The driver: 50 iterations, each catching the propagated exception.
std::string mainFor(const char* tag) {
    return std::string(R"(
  public static async method Main(): Task
    var caught := 0;
    for var i in 0..<50 do
      try
        await Program.R0();
      catch var e: InvalidOperationException =>
        caught := caught + 1;
      end;
    end for;
    Console.WriteLine($")") + tag +
           R"(: da={K.da} dt={K.dt} caught={caught} cells={Cell.n}");
  end Main;
end Program;
)";
}

// t2h_0_static: the static arm (`new N()` settles the class).
std::string kStaticHead = std::string(kPrelude) + R"(
public class Program
  public static async method R0(): Task
    await using var d := new N();
  end R0;
)" + mainFor("static");

// t2h_1_iad: the run-time arm behind an `owned IAsyncDisposable` call head.
std::string kIadHead = std::string(kPrelude) + R"(
public class Program
  public static method M0(): owned IAsyncDisposable return new N(); end M0;
  public static async method R0(): Task
    await using var d := Program.M0();
  end R0;
)" + mainFor("iad");

// t2h_2_id: the run-time arm behind an `owned IDisposable` call head.
std::string kIdHead = std::string(kPrelude) + R"(
public class Program
  public static method M0(): owned IDisposable return new N(); end M0;
  public static async method R0(): Task
    await using var d := Program.M0();
  end R0;
)" + mainFor("id");

// t2h_3_obj: the run-time arm behind a moved `Object` head.
std::string kObjHead = std::string(kPrelude) + R"(
public class Program
  public static async method R0(): Task
    var o: Object := new N();
    await using var d: Object := move o;
  end R0;
)" + mainFor("obj");

}  // namespace

// fix #3483 (§28.12 step 2 fault edge, v0.350): static head.
BEZEL_TEST(await_using_dispose_async_sync_throw,
           static_head_sync_throwing_dispose_async_reclaims_then_propagates_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kStaticHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "static: da=50 dt=0 caught=50 cells=0\n");
}

// fix #3483 (§28.12 step 2 fault edge, v0.350): `IAsyncDisposable` call head.
BEZEL_TEST(await_using_dispose_async_sync_throw,
           iasyncdisposable_head_sync_throwing_dispose_async_reclaims_then_propagates_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kIadHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "iad: da=50 dt=0 caught=50 cells=0\n");
}

// fix #3483 (§28.12 step 2 fault edge, v0.350): `IDisposable` call head.
BEZEL_TEST(await_using_dispose_async_sync_throw,
           idisposable_head_sync_throwing_dispose_async_reclaims_then_propagates_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kIdHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "id: da=50 dt=0 caught=50 cells=0\n");
}

// fix #3483 (§28.12 step 2 fault edge, v0.350): moved `Object` head.
BEZEL_TEST(await_using_dispose_async_sync_throw,
           object_head_sync_throwing_dispose_async_reclaims_then_propagates_spec_28_12_3483) {
    bezel::test::JitResult r = runAotChecked(kObjHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "obj: da=50 dt=0 caught=50 cells=0\n");
}
