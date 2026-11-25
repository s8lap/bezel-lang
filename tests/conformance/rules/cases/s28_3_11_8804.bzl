//! case: s28_3_11_8804
//! doc: spec
//! section: 28.3.11
//! title: a fresh class receiver of a generic method call is disposed when that method throws
//! rule: every already-produced, not-yet-consumed unbound owned temporary is disposed during unwinding by the same implicit `try`/`finally` mechanism as named locals
//! cites: 28.3.11, 28.3.10
//! expect: run
//! stdout: 600 600
//! notes: Pins #9106 (fix 4e5dbd3fe). `new Box()` and `Program.MkBox()` are unbound owned temporaries used as
//!  the receiver of the generic method `Thrower<Res>`, which throws; the bound local `y` is an owned local.
//!  Three Box per iteration, 600 in all, each disposed once.
module Conf;
public class Res
  public constructor() end constructor;
end Res;
public class Box
  public static N: int := 0;
  public static Made: int := 0;
  public constructor() Box.Made := Box.Made + 1; end constructor;
  destructor Box.N := Box.N + 1; end destructor;
  public method Thrower<T>(): int
    throw new InvalidOperationException("boom");
  end Thrower;
end Box;
public class Program
  public static method MkBox(): owned Box return new Box(); end MkBox;
  public static method Main(): int
    for var i in 0 ..< 200 do
      try
        var z := new Box().Thrower<Res>();
      catch InvalidOperationException =>
        var c := 0;
      end try;
      try
        var z := Program.MkBox().Thrower<Res>();
      catch InvalidOperationException =>
        var c := 0;
      end try;
      try
        using var y := new Box();
        var z := y.Thrower<Res>();
      catch InvalidOperationException =>
        var c := 0;
      end try;
    end for;
    Console.WriteLine($"{Box.N} {Box.Made}");
    return 0;
  end Main;
end Program;
