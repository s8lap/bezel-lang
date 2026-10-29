//! case: s28_12_0080
//! doc: spec
//! section: 28.12
//! title: three await using heads where the middle one faults still dispose all three through DisposeAsync
//! rule: Asynchronous cleanup runs only where the program wrote `await using` (whose implicit `finally` awaits `DisposeAsync` on both the normal and exception edges of an async body ... the awaited `DisposeAsync()` performs whatever asynchronous cleanup the class defines; the destructor body (if any) is not also run, because `DisposeAsync` supersedes it ... a throwing disposer does not skip the remaining values: every remaining disposal still runs. If a later disposal in the same pass also throws, the later exception supersedes the earlier one
//! cites: 28.4, 31.2
//! expect: run
//! output: boom=0 fboom=100 sboom=0 inner=0 canc=0 other=0 oda=300 odt=0 fin=0 mark=0 cells=0
//! issue: 7705
module Conf;
import System.Threading;
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
  public static sboom: int := 0;
  public static inner: int := 0;
  public static canc: int := 0;
  public static other: int := 0;
  public static fin: int := 0;
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
public class NSYNC: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(): Task
    K.oda := K.oda + 1;
    throw new ArgumentException("sboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NSYNC;
public class NAWF: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
    await Program.InnerAsync();
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NAWF;
public class NCATCH: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    try
      await Program.InnerAsync();
    catch var e: ArgumentException =>
      K.oda := K.oda + 1;
    end;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NCATCH;
public class NCAN: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
    using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromMilliseconds(1.0));
    await Task.Delay(5000, cts.Token);
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NCAN;
public interface IRunner
  method Run(k: int): Task;
end IRunner;
public class Program
  public static method MkI(): owned IAsyncDisposable return new NOK(); end MkI;
  public static method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
  public static method Id(c: Cell, k: int): int return k; end Id;
  public static method Take(owned c: Cell, k: int): int return k; end Take;
  public static async method BoomAsync(): Task
    await Task.Delay(1);
    throw new InvalidOperationException("boom");
  end BoomAsync;
  public static async method InnerAsync(): Task
    await Task.Delay(1);
    throw new ArgumentException("inner");
  end InnerAsync;
  public static method Tally(m: string)
    if m = "boom" then K.boom := K.boom + 1;
    elif m = "fboom" then K.fboom := K.fboom + 1;
    elif m = "sboom" then K.sboom := K.sboom + 1;
    elif m = "inner" then K.inner := K.inner + 1;
    elif m = "canc" then K.canc := K.canc + 1;
    else K.other := K.other + 1; end;
  end Tally;
  public static async method R(i: int): Task

    await using var a := new NOK();
    await using var b := new NFAULT();
    await using var c := new NOK();
    throw new InvalidOperationException("boom");
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: OperationCanceledException =>
        K.canc := K.canc + 1;
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} sboom={K.sboom} inner={K.inner} canc={K.canc} other={K.other} oda={K.oda} odt={K.odt} fin={K.fin} mark={K.mark} cells={Cell.n}");
  end Main;
end Program;
