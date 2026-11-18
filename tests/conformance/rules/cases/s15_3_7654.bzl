//! case: s15_3_7654
//! doc: spec
//! section: 15.3
//! title: a move-only value read through a method then passed on is disposed once per value
//! rule: for a move-only V, it is by value-move (transfer ownership)
//! cites: 15.3, 28.3.10
//! expect: run
//! stdout: 600
module Conf;
import System;
class Res
  public static N: int := 0;
  public constructor() end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;
choice OChoice Text(owned r: Res), Empty;
  public method Describe(): int => 7;
end OChoice;
class Program
  public static method Take(o: OChoice): int return 1; end Take;
  public static method Peek2(o: OChoice): int return o.Describe(); end Peek2;
  public static method T(o: OChoice): int
    var r := o.Describe();
    var q := Peek2(o);
    return r + q;
  end T;
  public static method T2(o: OChoice): int
    var r := o.Describe();
    var k := Take(o);
    return r + k;
  end T2;
  public static method Main()
    var i := 0;
    while i < 200 do
      var k := T(OChoice.Text(new Res()));
      var kk := Take(OChoice.Text(new Res()));
      var k2 := T2(OChoice.Text(new Res()));
      i := i + 1;
    end while;
    Console.WriteLine(Res.N);
  end Main;
end Program;
