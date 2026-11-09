// Codegen — issue #7888 (§28.12, v0.355): an ASYNC ITERATOR's body's exception
// edge awaits its `DisposeAsync`, like an async method's does.
//
//     public static async method Gen(): IAsyncEnumerable<int>
//       await using var o := new NOK();
//       yield return 0;
//       throw new InvalidOperationException("boom");     // the ITERATOR's edge
//     end Gen;
//
// §28.12, verbatim: asynchronous cleanup runs "where the program wrote
// `await using` (whose implicit `finally` awaits `DisposeAsync` on both the
// normal and exception edges of an async body ...)". §30.7's async iterator IS
// an async body — its body is lowered into a resume function with the same
// `smStateStruct_` / `smSelf_` / `asyncResumeFn_` machinery an ordinary async
// method has, and an `await` inside it lowers a real suspend point.
//
// The unwind pass's await-on-unwind gate named `smKind_ == SuspendKind::Await`,
// so an async iterator (`smKind_ == SuspendKind::AsyncYield`) took the
// SYNCHRONOUS fallback on its exception edge: the DESTRUCTOR BODY ran and
// `DisposeAsync` never did (MEASURED on this tree's base f0a1818e: the row
// below prints `oda=0 odt=100`). The expected output is `oda=100 odt=0`.
//
// The CONTROL rows pin the two edges that must NOT move:
//   * the iterator's NORMAL completion — the implicit `finally` awaits, and
//     did so before this fix;
//   * EARLY ABANDONMENT — the consumer throws inside the `await for` body, so
//     the iterator is abandoned and §29.7's SYNCHRONOUS fallback applies
//     ("with §29.7's synchronous fallback on early iterator abandonment"):
//     `oda=0 odt=100`, on the base and on the tip alike.
//
// The PER-LEVEL row is the §28.12 step 2 rule the whole family shares: the
// level that declares `DisposeAsync` has its destructor body SKIPPED, and a
// base level that declares none still has its destructor body RUN. A "skip all
// destructor bodies" shortcut would violate that rule and is rejected by
// this row.
//
// The row is deliberately the CONCRETE head only. The generic head's own
// destructor-body skip is a DIFFERENT rule (the per-instantiation
// reclamation of a generic `await using` head), it is not this issue's
// subject, and pinning it here would make this file assert a behaviour the
// tree does not have — so the generic twin is NOT part of this file.
//
// Every row runs the generated code under ASan + LeakSanitizer
// (runAotChecked), so a row that traded the destructor body for a LEAK would
// fail on the exit code, not only on the counters.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"BZ(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static oda: int := 0;
  public static odt: int := 0;
  public static caught: int := 0;
  public static other: int := 0;
  public static bdt: int := 0;
  public constructor() end constructor;
end K;
public class NOK: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NOK;
public class Program
)BZ";

// The ITERATOR's own body raises: the `await using` binding inside it is
// disposed on the ITERATOR's exception edge.
constexpr const char* kIterBodyThrow = R"BZ(
  public static async method Gen(): IAsyncEnumerable<int>
    await using var o := new NOK();
    yield return 0;
    throw new InvalidOperationException("boom");
  end Gen;
  public static async method R(): Task
    await for var v in Program.Gen() do
    end for;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R();
      catch var e: InvalidOperationException =>
        K.caught := K.caught + 1;
      end;
    end for;
)BZ";

// The iterator completes NORMALLY: the implicit `finally` awaits.
constexpr const char* kIterNormal = R"BZ(
  public static async method Gen(): IAsyncEnumerable<int>
    await using var o := new NOK();
    yield return 0;
    yield return 1;
  end Gen;
  public static async method R(): Task
    await for var v in Program.Gen() do
    end for;
  end R;
  public static async method Main(): Task
    for var i in 0..<10 do
      await Program.R();
    end for;
)BZ";

// The CONSUMER throws inside the `await for` body: the iterator is abandoned
// early and §29.7's synchronous fallback applies.
constexpr const char* kIterAbandoned = R"BZ(
  public static async method Gen(): IAsyncEnumerable<int>
    await using var o := new NOK();
    yield return 0;
    yield return 1;
  end Gen;
  public static async method R(): Task
    await for var v in Program.Gen() do
      throw new InvalidOperationException("boom");
    end for;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R();
      catch var e: InvalidOperationException =>
        K.caught := K.caught + 1;
      end;
    end for;
)BZ";

std::string build(const char* body) {
    return std::string(kPrelude) + body + R"BZ(
    Console.WriteLine($"caught={K.caught} other={K.other} oda={K.oda} odt={K.odt} bdt={K.bdt} cells={Cell.n}");
  end Main;
end Program;
)BZ";
}

void expectOut(const std::string& src, const char* want) {
    bezel::test::JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's shape: the ASYNC ITERATOR's body's exception edge. The awaited
// `DisposeAsync` runs (oda == 100) and the destructor body does NOT
// (odt == 0), and the owned `Cell` was cascaded (cells == 0).
BEZEL_TEST(await_using_async_iterator_exception_edge,
           async_iterator_body_throw_awaits_dispose_async_spec_28_12_7888) {
    expectOut(build(kIterBodyThrow),
              "caught=100 other=0 oda=100 odt=0 bdt=0 cells=0\n");
}

// CONTROL: the iterator's NORMAL completion awaited before this fix and must
// still await.
BEZEL_TEST(await_using_async_iterator_exception_edge,
           async_iterator_normal_completion_awaits_dispose_async_control_7888) {
    expectOut(build(kIterNormal),
              "caught=0 other=0 oda=10 odt=0 bdt=0 cells=0\n");
}

// CONTROL: EARLY ABANDONMENT keeps §29.7's synchronous fallback — the
// destructor body runs and `DisposeAsync` does not.
BEZEL_TEST(await_using_async_iterator_exception_edge,
           early_iterator_abandonment_keeps_the_sync_fallback_control_7888) {
    expectOut(build(kIterAbandoned),
              "caught=100 other=0 oda=0 odt=100 bdt=0 cells=0\n");
}

// The PER-LEVEL rule (§28.12 step 2) on the CONCRETE head: the level that
// declares `DisposeAsync` has its destructor body skipped; the base level,
// which declares none, still has its destructor body run.
BEZEL_TEST(await_using_async_iterator_exception_edge,
           per_level_concrete_head_skips_only_the_async_level_control_7888) {
    expectOut(R"BZ(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static oda: int := 0;
  public static odt: int := 0;
  public static bdt: int := 0;
  public constructor() end constructor;
end K;
public class Base
  public owned _b: Cell;
  public constructor() _b := new Cell(); end constructor;
  destructor K.bdt := K.bdt + 1; end destructor;
end Base;
public class Der: Base, IAsyncDisposable
  public owned _d: Cell;
  public constructor() base(); _d := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end Der;
public class Program
  public static async method Run(): Task
    await using var o := new Der();
  end Run;
  public static async method Main(): Task
    await Program.Run();
    Console.WriteLine($"oda={K.oda} odt={K.odt} bdt={K.bdt} cells={Cell.n}");
  end Main;
end Program;
)BZ",
              "oda=1 odt=0 bdt=1 cells=0\n");
}
