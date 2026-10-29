// Codegen — issue #7417 (§28.12, v0.350): an `await using` head whose static
// type is a base class that does NOT implement `IAsyncDisposable`, holding a
// derived object whose class does,
//
//     public class Der: Base, IAsyncDisposable ... end Der;
//     public static method Make(): owned Base return new Der(); end Make;
//     ...
//     await using var b: Base := Program.Make();
//
// took the synchronous disposal (`body DER_DTOR BASE_DTOR`) and never awaited
// `Der.DisposeAsync`. §28.12 v0.350, verbatim:
//
//   "The `DisposeAsync` awaited is the run-time object's. `await using`
//    selects the logical-disposal path by the class of the object the binding
//    holds ... never by the binding's static type."
//
// and it names this static type explicitly: "through a base-class static type
// that does not implement `IAsyncDisposable`". Step 2 fixes the order: "for
// `class Der: Base, IAsyncDisposable` where `Base` declares no `DisposeAsync`,
// `await using` runs `Der.DisposeAsync`, skips `Der`'s destructor, and runs
// `Base`'s". The fault edge: "when `DisposeAsync` completes by throwing ...
// the runtime runs the cascade and the chain, then the exception propagates
// from the await".
//
// The mechanism is #3483's (the run-time `IAsyncDisposable` itable entry, the
// #3493 owner ruling's option 2); these rows pin the base-class head, which
// static resolution cannot reach because `Base` names no `DisposeAsync`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The issue's literal repro.
constexpr const char* kLiteral = R"(
module M;
public class Base
  public constructor()
  end constructor;
  destructor
    Console.WriteLine("BASE_DTOR");
  end destructor;
end Base;

public class Der: Base, IAsyncDisposable
  public constructor()
  end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor
    Console.WriteLine("DER_DTOR");
  end destructor;
end Der;

public class Program
  public static method Make(): owned Base
    return new Der();
  end Make;

  public static async method Main(): Task
    await using var b: Base := Program.Make();
    Console.WriteLine("body");
  end Main;
end Program;
)";

// 60 iterations, an owned `Cell` at each level: step 2's order as counters,
// and the cascade at BOTH levels (no stranded `Cell`, no ASan leak).
constexpr const char* kLoopedCascade = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class Base
  public static bdt: int := 0;
  public owned _b: Cell;
  public constructor() _b := new Cell(); end constructor;
  destructor Base.bdt := Base.bdt + 1; end destructor;
end Base;
public class Der: Base, IAsyncDisposable
  public static da: int := 0;
  public static ddt: int := 0;
  public owned _d: Cell;
  public constructor() _d := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Der.da := Der.da + 1;
  end DisposeAsync;
  destructor Der.ddt := Der.ddt + 1; end destructor;
end Der;
public class Program
  public static method Make(): owned Base return new Der(); end Make;
  public static async method Run(): Task
    await using var b: Base := Program.Make();
  end Run;
  public static async method Main(): Task
    for var i in 0..<60 do await Program.Run(); end for;
    Console.WriteLine($"da={Der.da} ddt={Der.ddt} bdt={Base.bdt} cells={Cell.n}");
  end Main;
end Program;
)";

// The fault edge: `DisposeAsync` throws; the chain (`BASE_DTOR`) and the
// cascade (`cells=0`) run, then the exception propagates from the await.
constexpr const char* kFaultEdge = R"(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class Base
  public constructor() end constructor;
  destructor Console.WriteLine("BASE_DTOR"); end destructor;
end Base;
public class Der: Base, IAsyncDisposable
  public owned _d: Cell;
  public constructor() _d := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
    throw new InvalidOperationException("boom");
  end DisposeAsync;
  destructor Console.WriteLine("DER_DTOR"); end destructor;
end Der;
public class Program
  public static method Make(): owned Base return new Der(); end Make;
  public static async method Run(): Task
    await using var b: Base := Program.Make();
    Console.WriteLine("body");
  end Run;
  public static async method Main(): Task
    try
      await Program.Run();
    catch var e: InvalidOperationException =>
      Console.WriteLine("caught " + e.Message);
    end;
    Console.WriteLine($"cells={Cell.n}");
  end Main;
end Program;
)";

// The head is a moved `owned Base` parameter and a moved `Base` local — no
// call RHS at all; the static type alone hides `DisposeAsync`.
constexpr const char* kMovedBaseHeads = R"(
module M;
public class Base
  public constructor() end constructor;
  destructor Console.WriteLine("BASE_DTOR"); end destructor;
end Base;
public class Der: Base, IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("DER_DTOR"); end destructor;
end Der;
public class Program
  public static async method Take(b: owned Base): Task
    await using var x := move b;
    Console.WriteLine("body");
  end Take;
  public static async method Main(): Task
    await Program.Take(new Der());
    var l: Base := new Der();
    await using var y := move l;
    Console.WriteLine("body2");
  end Main;
end Program;
)";

}  // namespace

// fix #7417 (§28.12 step 2, v0.350): the literal repro.
// Before: `body\nDER_DTOR\nBASE_DTOR\n`.
BEZEL_TEST(await_using_base_typed_runtime_dispose_async,
           base_typed_call_head_awaits_derived_dispose_async_spec_28_12_7417) {
    bezel::test::JitResult r = runAotChecked(kLiteral);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\nBASE_DTOR\n");
}

// fix #7417 (§28.12 step 2, v0.350): 60 iterations.
// Before: `da=0 ddt=60 bdt=60 cells=0`.
BEZEL_TEST(await_using_base_typed_runtime_dispose_async,
           base_typed_head_looped_supersedes_only_derived_level_spec_28_12_7417) {
    bezel::test::JitResult r = runAotChecked(kLoopedCascade);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "da=60 ddt=0 bdt=60 cells=0\n");
}

// fix #7417 (§28.12 fault edge, v0.350): reclamation, then propagation.
BEZEL_TEST(await_using_base_typed_runtime_dispose_async,
           base_typed_head_throwing_dispose_async_reclaims_then_propagates_spec_28_12_7417) {
    bezel::test::JitResult r = runAotChecked(kFaultEdge);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\nBASE_DTOR\ncaught boom\ncells=0\n");
}

// fix #7417 (§28.12, v0.350): moved parameter and moved local heads.
BEZEL_TEST(await_using_base_typed_runtime_dispose_async,
           moved_base_typed_heads_await_derived_dispose_async_spec_28_12_7417) {
    bezel::test::JitResult r = runAotChecked(kMovedBaseHeads);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "body\nDA\nBASE_DTOR\nbody2\nDA\nBASE_DTOR\n");
}
