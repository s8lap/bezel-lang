// Codegen — issue #8470 (§30.9 / §30.14, v0.357): the fault of a `Domain.Spawn`
// entry does not cross the boundary as an object; the join raises
// DomainFaultedException carrying copies of the message and stack-trace text
// (§30.14 *Faults*). The ORIGINAL's handling therefore ends at the wrap — and
// the end of a handling disposes only an instance nothing else owns.
//
//     private static _cache: Task<int>? := null;
//     public static method EntryCache(): int      // the Domain.Spawn entry
//       if _cache = null then _cache := BoomNowAsync(); end;
//       return _cache!.Result;                     // re-raises the task-owned instance
//     end EntryCache;
//
// §30.9, v0.357, *Fault ownership*, verbatim: "Awaiting a faulted task (or
// reading a faulted `Result`) re-raises the task-owned instance"; "The
// exception is disposed exactly once, by the task that owns it, at the later
// of the task's disposal and its settlement".
//
// The runtime's fault wrap (bz_domain_spawn) disposed the entry's in-flight
// exception unconditionally. MEASURED on r30-c1 4a6919f1: an entry that faults
// with a cached task's instance frees it under the task, and the next entry to
// read that task re-raises freed heap (`static_cached_task`, ASan
// heap-use-after-free). When the owner is a local disposed in the entry's
// unwind, its disposal had been deferred to the end of the handling (§30.9)
// and the wrap's direct dispose left the hold and the deferral on record for
// the next instance the allocator places at that address — invisible while
// ASan quarantines freed blocks, a crash in a release build
// (`local_task_owned_then_bound_reawait`; probe i01 of the lane, measured with
// the quarantine disabled).
//
// The wrap now ends the handling the way every other path does
// (bz_exc_end_handling) and disposes only when that says so.
//
// Oracle: `MyEx` counts constructions (`xmade`) and destructions (`xgone`).
// Every row loops 100x under generated-code ASan (runAotChecked).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"BZ(module M;
public class XC
  public static made: int := 0;
  public static gone: int := 0;
  public constructor() end constructor;
end XC;
public class MyEx: Exception
  public constructor(m: string) base(m); XC.made := XC.made + 1; end constructor;
  destructor XC.gone := XC.gone + 1; end destructor;
end MyEx;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static oda: int := 0;
  public static odt: int := 0;
  public static boom: int := 0;
  public static fboom: int := 0;
  public static inner: int := 0;
  public static other: int := 0;
  public static mark: int := 0;
  public constructor() end constructor;
end K;
public class NOK: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NOK;
public class NFAULT: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
    throw new ArgumentException("fboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NFAULT;
public class DThrow
  public constructor() end constructor;
  destructor
    throw new ArgumentException("inner");
  end destructor;
end DThrow;
public class CThrow
  public constructor()
    throw new ArgumentException("inner");
  end constructor;
end CThrow;
public class H
  public owned t: Task;
  public constructor(owned tk: Task) t := tk; end constructor;
end H;
public class Program
  public static method Boom(): int
    throw new ArgumentException("inner");
  end Boom;
  public static method GuardBoom(): bool
    throw new ArgumentException("inner");
  end GuardBoom;
  public static async method BoomAsync(): Task
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomAsync;
  public static async method InnerAsync(): Task
    await Task.Delay(1);
    throw new ArgumentException("inner");
  end InnerAsync;
  public static async method BoomIntAsync(): Task<int>
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomIntAsync;
  public static async method BoomNowAsync(): Task<int>
    throw new MyEx("boom");
  end BoomNowAsync;
  public static method SyncRead(t: Task<int>): int
    return t.Result;
  end SyncRead;
  public static method SyncOwn(): int
    var t := Program.BoomNowAsync();
    return t.Result;
  end SyncOwn;
  public static method SyncNested(): int
    var t := Program.BoomNowAsync();
    try
      var x := t.Result;
    catch var e: MyEx =>
      do
        try
          var y := t.Result;
        catch var e2: MyEx =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
      end;
    end;
    return 0;
  end SyncNested;
  public static method Tally(m: string)
    if m = "boom" then K.boom := K.boom + 1;
    elif m = "fboom" then K.fboom := K.fboom + 1;
    elif m = "inner" then K.inner := K.inner + 1;
    else K.other := K.other + 1; end;
  end Tally;
)BZ";

void expectOut(const char* programTail, const char* want) {
    bezel::test::JitResult r =
        runAotChecked(std::string(kPrelude) + programTail);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The repro: the entry reads a static cached faulted task (the lazy task
// cache idiom). ONE instance for the whole run, owned by the cached task:
// `xmade=1`, and never disposed by a join.
BEZEL_TEST(domain_fault_wrap_borrowed_instance_8470,
           spec_30_9_entry_faults_with_static_cached_task_instance) {
    expectOut(R"BZ(  private static _cache: Task<int>? := null;
  public static method EntryCache(): int
    if Program._cache = null then
      Program._cache := Program.BoomNowAsync();
    end;
    return Program._cache!.Result;
  end EntryCache;
  public static async method R(i: int): Task
    using var j := Domain.Spawn(Program.EntryCache);
    try
      var x := await j;
    catch var e: DomainFaultedException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=1 xgone=0\n");
}

// Kept control (review r42-rev-f1-7943 F2; green on the parent while ASan
// quarantines the freed block, see the header). The owner is the entry's
// local: the wrap performs its deferred disposal once. Wrong fix it catches:
// the wrap disposing the instance unconditionally -> a double dispose
// (xgone > xmade).
BEZEL_TEST(domain_fault_wrap_borrowed_instance_8470,
           spec_30_9_entry_faults_with_local_task_owned_instance_then_bound_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    using var j := Domain.Spawn(Program.SyncOwn);
    try
      var x := await j;
    catch var e: DomainFaultedException =>
      K.mark := K.mark + 1;
    end;
    var t := Program.BoomAsync();
    try await t; catch var e2: MyEx => Program.Tally(e2.Message); end;
    try await t; catch var e3: MyEx => Program.Tally(e3.Message); end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// Kept control (review r42-rev-f1-7943 F2; green on the parent). A fresh
// handler-owned fault is disposed by the wrap exactly once. Wrong fix it
// catches: the wrap ending the handling WITHOUT disposing (the cure for
// the borrowed-instance UAF applied to every instance) -> xgone=0, a leak.
BEZEL_TEST(domain_fault_wrap_borrowed_instance_8470,
           spec_30_14_entry_faults_with_fresh_instance) {
    expectOut(R"BZ(  public static method EntryFresh(): int
    throw new MyEx("boom");
  end EntryFresh;
  public static async method R(i: int): Task
    using var j := Domain.Spawn(Program.EntryFresh);
    try
      var x := await j;
    catch var e: DomainFaultedException =>
      K.mark := K.mark + 1;
    end;
    var t := Program.BoomAsync();
    try await t; catch var e2: MyEx => Program.Tally(e2.Message); end;
    try await t; catch var e3: MyEx => Program.Tally(e3.Message); end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}
