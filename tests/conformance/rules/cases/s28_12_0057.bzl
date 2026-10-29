//! case: s28_12_0057
//! doc: spec
//! section: 28.12
//! title: await using in a finally awaits DisposeAsync of a holder whose task faulted; the fault surfaces
//! rule: `await using` awaits the run-time object's DisposeAsync on both the normal and the exception edges of an async body, and the destructor body is skipped at a level that declares DisposeAsync (spec 28.12 step 1 and "Implicit discharge is synchronous", v0.350). The cascade and the chain run after DisposeAsync whichever way the scope ended.
//! cites: 28.12
//! expect: run
//! stdout: p01_shared_task_finally: K: da=0 dt=0 ada=100 adt=0 oda=0 odt=0 c1=100 c2=0 c3=0 cells=0
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static da: int := 0;
  public static dt: int := 0;
  public static ada: int := 0;
  public static adt: int := 0;
  public static oda: int := 0;
  public static odt: int := 0;
  public static c1: int := 0;
  public static c2: int := 0;
  public static c3: int := 0;
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
public class NA: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.ada := K.ada + 1;
    throw new InvalidOperationException("aboom");
  end DisposeAsync;
  destructor K.adt := K.adt + 1; end destructor;
end NA;
public class NOK: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NOK;
public class W: IAsyncDisposable
  public owned _t: Task;
  public constructor(owned t: Task) _t := t; end constructor;
  public async method DisposeAsync(): Task
    K.ada := K.ada + 1;
    await this._t;
  end DisposeAsync;
end W;
public class Thr
  public constructor() end constructor;
  destructor K.dt := K.dt + 1; throw new InvalidOperationException("boom"); end destructor;
end Thr;
public class Program
  public static method MkI(): owned IAsyncDisposable return new N(); end MkI;
  public static method MkAI(): owned IAsyncDisposable return new NA(); end MkAI;
  public static method Tally(m: string, ok: string) if m = ok then K.c1 := K.c1 + 1; else K.c2 := K.c2 + 1; end; end Tally;
  public static async method Boom(): Task
    throw new InvalidOperationException("E1");
  end Boom;
  public static method Use(s: string) if s.Length = 0 then K.c2 := K.c2 + 1000; end; end Use;
  public static async method R(i: int): Task
    var w := new W(Program.Boom());
    try
      await w._t;
    finally
      do
        await using var d := move w;
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: InvalidOperationException =>
        Program.Tally(e.Message, "E1");
      catch var e: ArgumentException =>
        K.c3 := K.c3 + 1;
      end;
    end for;
    Console.WriteLine($"p01_shared_task_finally: K: da={K.da} dt={K.dt} ada={K.ada} adt={K.adt} oda={K.oda} odt={K.odt} c1={K.c1} c2={K.c2} c3={K.c3} cells={Cell.n}");
  end Main;
end Program;
