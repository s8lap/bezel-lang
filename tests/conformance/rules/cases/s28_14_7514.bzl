//! case: s28_14_7514
//! doc: spec
//! section: 28.14
//! title: a scoped field is overwritten and then cleared, and the cleared field invokes nothing
//! rule: **overwritten**, or **cleared**, but never read out as an escaping value
//! cites: 28.14
//! expect: run
//! stdout: -2
module Conf;
import System;
public class Holder
  private scoped _cb: (() -> int)?;
  public method Reg(scoped f: () -> int)
    _cb := f;
  end Reg;
  public method Clear()
    _cb := null;
  end Clear;
  public method Fire(): int => _cb?() ?? -5;
end Holder;
public class Program
  public static method Main()
    var h := new Holder();
    var n := 0; var i := 0;
    while i < 3 do
      var k := i;
      h.Reg(lambda() => k);
      n := n + h.Fire();
      i := i + 1;
    end;
    h.Clear();
    h.Reg(lambda() => 3);
    h.Clear();
    Console.WriteLine(n + h.Fire());
  end Main;
end Program;
