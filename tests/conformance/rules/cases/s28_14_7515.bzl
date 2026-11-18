//! case: s28_14_7515
//! doc: spec
//! section: 28.14
//! title: a static and a bound method reference are stored in a scoped field of a using var holder
//! rule: where every captured local source is declared before `L` in the same or an enclosing scope
//! cites: 28.14, 28.3
//! expect: run
//! stdout: 39
module Conf;
import System;
public class Counter
  public v: int := 4;
  public method Get(): int => v;
  public static method S(): int => 9;
end Counter;
public class Holder
  private scoped _cb: (() -> int)?;
  public method Reg(scoped f: () -> int)
    _cb := f;
  end Reg;
  public method Fire(): int => _cb?() ?? -5;
end Holder;
public class Program
  public static method Main()
    var n := 0; var i := 0;
    while i < 3 do
      var c := new Counter();
      using var h := new Holder();
      h.Reg(Counter.S);
      n := n + h.Fire();
      h.Reg(c.Get);
      n := n + h.Fire();
      i := i + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
