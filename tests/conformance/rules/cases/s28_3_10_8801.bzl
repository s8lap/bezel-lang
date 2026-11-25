//! case: s28_3_10_8801
//! doc: spec
//! section: 28.3.10
//! title: a generic method's owned local is disposed when a generic callee throws through it
//! rule: Owned locals and owned parameters that are live when an exception propagates out of their enclosing block are disposed implicitly during stack unwinding
//! cites: 28.3.10, 28.3.11
//! expect: run
//! stdout: 200 200
//! notes: Pins #9106 (fix 4e5dbd3fe). Each of 200 calls constructs one Res, whose `using var` local is live
//!  when the generic callee `Thrower<T>` throws; the unwind disposes it, so the destructor count
//!  equals the construction count. Without the fix the count printed first stays 0.
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
  public static method Chain<T>(): int
    using var r := new Res();
    return G.Thrower<T>();
  end Chain;
end G;
public class Program
  public static method Main(): int
    for var i in 0 ..< 200 do
      try
        var z := G.Chain<Res>();
      catch InvalidOperationException =>
        var c := 0;
      end try;
    end for;
    Console.WriteLine($"{Res.N} {Res.Made}");
    return 0;
  end Main;
end Program;
