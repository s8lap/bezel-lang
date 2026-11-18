//! case: s28_14_7510
//! doc: spec
//! section: 28.14
//! title: a scoped callback is invoked in the callee after two scoped fields are stored and overwritten
//! rule: it may invoke it
//! cites: 28.14
//! expect: run
//! stdout: 12
module Conf;
import System;
public class Holder
  private scoped _cb: (() -> int)?;
  private scoped _cb2: (() -> int)?;
  public method Reg(scoped f: () -> int, scoped g: () -> int): int
    _cb := f;
    _cb2 := g;
    _cb := g;
    return f() + g();
  end Reg;
  public method Fire(): int => (_cb?() ?? -5) + (_cb2?() ?? -5);
end Holder;
public class Program
  public static method Main()
    var n := 0; var i := 0;
    while i < 3 do
      var h := new Holder();
      var k := i;
      n := n + h.Reg(lambda() => 1, lambda() => k);
      n := n + h.Fire();
      i := i + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
