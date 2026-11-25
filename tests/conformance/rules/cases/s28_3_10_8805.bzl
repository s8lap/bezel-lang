//! case: s28_3_10_8805
//! doc: spec
//! section: 28.3.10
//! title: an async method's owned local and generic receiver temporary are disposed on a throw
//! rule: Owned locals and owned parameters that are live when an exception propagates out of their enclosing block are disposed implicitly during stack unwinding
//! cites: 28.3.10, 28.3.11
//! expect: run
//! stdout: 800 800
//! notes: Pins #9106 (fix 4e5dbd3fe). The async twin of 8802/8803, once with no await and once with an await before
//!  the throw. Per call the `using var l` and the `h.MakeW()` temporary are two Res; 2 calls per iteration,
//!  200 iterations: 800.
module Conf;
public class Res
  public static N: int := 0;
  public static Made: int := 0;
  public constructor() Res.Made := Res.Made + 1; end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;
public choice Wrap<T> Has(owned v: T), None;
  public method Count(): int => (case this of Has(v) => 1, None => 0);
end Wrap;
public class H
  public constructor() end constructor;
  public method MakeW(): Wrap<Res> return Wrap<Res>.Has(new Res()); end MakeW;
end H;
public class G
  public static method Thrower<T>(): int
    throw new InvalidOperationException("boom");
  end Thrower;
end G;
public class A
  public static async method Run(h: H): Task<int>
    try
      using var l := new Res();
      var z := h.MakeW().Count() + G.Thrower<Res>();
      return z;
    catch InvalidOperationException =>
      return 0;
    end try;
  end Run;
  public static async method Run2(h: H): Task<int>
    try
      using var l := new Res();
      await Task.Delay(1);
      var z := h.MakeW().Count() + G.Thrower<Res>();
      return z;
    catch InvalidOperationException =>
      return 0;
    end try;
  end Run2;
end A;
public class Program
  public static async method Main(): Task
    var h := new H();
    for var i in 0 ..< 200 do
      var a := await A.Run(h);
      var b := await A.Run2(h);
    end for;
    Console.WriteLine($"{Res.N} {Res.Made}");
  end Main;
end Program;
