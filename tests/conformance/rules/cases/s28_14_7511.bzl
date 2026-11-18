//! case: s28_14_7511
//! doc: spec
//! section: 28.14
//! title: two scoped parameters stored, one forwarded to a scoped parameter of this, then replaced
//! rule: forward it to another `scoped` parameter whose bounding object is the same object
//! cites: 28.14
//! expect: run
//! stdout: 318
module Conf;
import System;
public class Holder
  private scoped _cb: (() -> int)?;
  private scoped _cb2: (() -> int)?;
  public method Reg(scoped f: () -> int)
    _cb := f;
  end Reg;
  public method RegBoth(scoped f: () -> int, scoped g: () -> int)
    _cb := f;
    _cb2 := g;
    this.Reg(g);
  end RegBoth;
  public method Fire(): int => (_cb?() ?? -5) + (_cb2?() ?? -5);
end Holder;
public class Program
  public static method Main()
    var n := 0; var i := 0;
    var h := new Holder();
    while i < 3 do
      var k := i;
      h.RegBoth(lambda() => k, lambda() => k + 1);
      n := n + h.Fire();
      h.Reg(lambda() => 100);
      n := n + h.Fire();
      i := i + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
