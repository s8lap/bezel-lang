//! case: s28_3_10_8806
//! doc: spec
//! section: 28.3.10
//! title: an iterator body's owned local and generic receiver temporary are disposed on a throw
//! rule: Owned locals and owned parameters that are live when an exception propagates out of their enclosing block are disposed implicitly during stack unwinding
//! cites: 28.3.10, 28.3.11
//! expect: run
//! stdout: 800 800 21600
//! notes: Pins #9106 (fix 4e5dbd3fe). The iterator twin of 8802/8803. Per iteration the body yields 1, then a
//!  try whose `using var l` and `MakeW()` temporary (two Res) are unwound by the throw into the catch (z := 5),
//!  yields 5, then a second try with the same two Res that yields 2 and throws out of the iterator into the
//!  caller's catch. Sum per iteration 1 + 5 + 2 + 100 = 108, times 200 is 21600; 4 Res per iteration, 800.
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
  public static method Gen(h: H): IEnumerable<int>
    var z := 0;
    yield return 1;
    try
      using var l := new Res();
      z := h.MakeW().Count() + G.Thrower<Res>();
    catch InvalidOperationException =>
      z := 5;
    end try;
    yield return z;
    try
      using var l2 := new Res();
      yield return 2;
      z := h.MakeW().Count() + G.Thrower<Res>();
    finally
      var f := 0;
    end try;
    yield return 3;
  end Gen;
end A;
public class Program
  public static method Main(): int
    var h := new H();
    var s := 0;
    for var i in 0 ..< 200 do
      try
        for var x in A.Gen(h) do
          s := s + x;
        end for;
      catch InvalidOperationException =>
        s := s + 100;
      end try;
    end for;
    Console.WriteLine($"{Res.N} {Res.Made} {s}");
    return 0;
  end Main;
end Program;
