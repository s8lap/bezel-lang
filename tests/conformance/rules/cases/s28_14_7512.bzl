//! case: s28_14_7512
//! doc: spec
//! section: 28.14
//! title: a stored scoped callback re-enters another holder while its own holder invokes it
//! rule: The callee may store the callback
//! cites: 28.14
//! expect: run
//! stdout: 33
module Conf;
import System;
public class Holder
  private scoped _cb: (() -> int)?;
  public method Reg(scoped f: () -> int): int
    _cb := f;
    return f();
  end Reg;
  public method Fire(): int => _cb?() ?? -5;
end Holder;
public class Program
  public static method Main()
    var n := 0; var i := 0;
    while i < 3 do
      using var h2 := new Holder();
      using var h := new Holder();
      n := n + h.Reg(lambda() => h2.Reg(lambda() => 3) + 1);
      n := n + h.Fire() + h2.Fire();
      i := i + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
