//! case: s29_7_0008
//! doc: spec
//! section: 29.7
//! title: an exception thrown in an await for body runs the abandoned iterator's finally once
//! rule: On break, on an exception thrown from the body, or on any other early exit from the for-loop, the enumerator is disposed, and a source-written finally in the iterator runs (spec 29.2, 29.7); the iterator's frame is reclaimed exactly once.
//! cites: 29.2, 31.2
//! expect: run
//! output: boom=20 fin=20 cells=0
//! issue: 7690
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
  public static async method Gen(): IAsyncEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      K.c3 := K.c3 + 1;
    end;
  end Gen;

  public static async method Main(): Task
    for var i in 0..<20 do
      try
        await for var x in Program.Gen() do
          throw new InvalidOperationException("boom");
        end;
      catch var e: InvalidOperationException =>
        K.boom := K.boom + 1;
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fin={K.c3} cells={Cell.n}");
  end Main;
end Program;
