//! case: s28_12_0051
//! doc: spec
//! section: 28.12
//! title: a string declared after a throwing await using binding is released exactly once
//! rule: A local declared after the binding is released exactly once when the binding's DisposeAsync throws at scope exit (spec 28.4, 28.2 exactly-once); An exception raised by the awaited DisposeAsync propagates out of the enclosing async body and leaves no resource behind: every disposal is exactly-once and the destructor body is superseded by DisposeAsync (spec 28.12 steps 1 and 2, 28.4).
//! cites: 28.4, 31.2
//! expect: run
//! output: da=20 dt=0 oda=0 odt=0 boom=20 other=0 c3=0 cells=0
//! issue: 7630
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
  public static async method R(i: int): Task
    await using var d := new N();
    var s1 := $"post{i}";
    K.c3 := K.c3 + s1.Length - s1.Length;
  end R;

  public static async method Main(): Task
    for var i in 0..<20 do
      try
        await Program.R(i);
      catch var e: InvalidOperationException =>
        K.boom := K.boom + 1;
      catch var e: ArgumentException =>
        K.other := K.other + 1;
      end;
    end for;
    Console.WriteLine($"da={K.da} dt={K.dt} oda={K.oda} odt={K.odt} boom={K.boom} other={K.other} c3={K.c3} cells={Cell.n}");
  end Main;
end Program;
