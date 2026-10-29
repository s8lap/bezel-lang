//! case: s28_12_0047
//! doc: spec
//! section: 28.12
//! title: a throwing DisposeAsync via a generic parameter and an interface head reclaims
//! rule: An exception raised by the awaited DisposeAsync propagates out of the enclosing async body after the remaining scope-exit disposals have run (await using's implicit finally awaits DisposeAsync on both the normal and exception edges, spec 28.12; a throwing disposer does not skip the remaining disposals, spec 28.4). Where the run-time class implements IAsyncDisposable, scope exit of an await using awaits its DisposeAsync and reclamation follows as step 2 states: owned fields are disposed whichever path ran, and the destructor body is skipped at a level that declares a non-abstract DisposeAsync (spec 28.12).
//! cites: 28.4, 31.2
//! expect: run
//! output: da=20 dt=0 oda=0 odt=0 boom=20 other=0 c3=0 cells=0
module Conf;

public class Cell
  public static n: int := 0;

  public constructor()
    Cell.n := Cell.n + 1;
  end constructor;

  destructor
    Cell.n := Cell.n - 1;
  end destructor;
end Cell;

public class K
  public static da: int := 0;
  public static dt: int := 0;
  public static oda: int := 0;
  public static odt: int := 0;
  public static boom: int := 0;
  public static other: int := 0;
  public static c3: int := 0;
end K;

public class N: IAsyncDisposable
  public owned _c: Cell;

  public constructor()
    _c := new Cell();
  end constructor;

  public method DisposeAsync(): Task
    K.da := K.da + 1;
    throw new InvalidOperationException("boom");
  end DisposeAsync;

  destructor
    K.dt := K.dt + 1;
  end destructor;
end N;

public class NOK: IAsyncDisposable
  public owned _c: Cell;

  public constructor()
    _c := new Cell();
  end constructor;

  public async method DisposeAsync(): Task
    K.oda := K.oda + 1;
  end DisposeAsync;

  destructor
    K.odt := K.odt + 1;
  end destructor;
end NOK;

public class Program
  public static method MkI(): owned IAsyncDisposable
    return new N();
  end MkI;
  public static async method Use<T>(x: owned T): Task where T: IAsyncDisposable
    await using var d := move x;
  end Use;

  public static async method R(): Task
    await Program.Use(Program.MkI());
  end R;

  public static async method Main(): Task
    for var i in 0..<20 do
      try
        await Program.R();
      catch var e: InvalidOperationException =>
        K.boom := K.boom + 1;
      catch var e: ArgumentException =>
        K.other := K.other + 1;
      end;
    end for;
    Console.WriteLine($"da={K.da} dt={K.dt} oda={K.oda} odt={K.odt} boom={K.boom} other={K.other} c3={K.c3} cells={Cell.n}");
  end Main;
end Program;
