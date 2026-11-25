//! case: s28_3_10_8802
//! doc: spec
//! section: 28.3.10
//! title: a caller's owned local is disposed when a generic method called in the same try throws
//! rule: Owned locals and owned parameters that are live when an exception propagates out of their enclosing block are disposed implicitly during stack unwinding
//! cites: 28.3.10, 28.3.11
//! expect: run
//! stdout: 200 200
//! notes: Pins #9106 (fix 4e5dbd3fe). The `using var l` of each try body is live when `Thrower<Res>()`
//!  throws; one Res is made per iteration and the unwind disposes it.
module Conf;
public class Res
  public static N: int := 0;
  public static Made: int := 0;
  public constructor() Res.Made := Res.Made + 1; end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;
public class G
  public static method Thrower<T>(): int
    throw new InvalidOperationException("boom");
  end Thrower;
end G;
public class Program
  public static method Main(): int
    for var i in 0 ..< 200 do
      try
        using var l := new Res();
        var z := G.Thrower<Res>();
      catch InvalidOperationException =>
        var c := 0;
      end try;
    end for;
    Console.WriteLine($"{Res.N} {Res.Made}");
    return 0;
  end Main;
end Program;
