//! case: s28_14_7513
//! doc: spec
//! section: 28.14
//! title: a scoped callback forwarded through two scoped parameters of this is stored once and still invoked
//! rule: but only in `scoped` fields **of the bounding object itself** (below)
//! cites: 28.14
//! expect: run
//! stdout: 21
module Conf;
import System;
public class Holder
  private scoped _cb: (() -> int)?;
  public method Reg(scoped f: () -> int)
    _cb := f;
  end Reg;
  public method R2(scoped f: () -> int)
    this.Reg(f);
  end R2;
  public method R3(scoped f: () -> int)
    this.R2(f);
    _ := f();
  end R3;
  public method Fire(): int => _cb?() ?? -5;
end Holder;
public class Program
  public static method Main()
    var n := 0; var i := 0;
    while i < 3 do
      var h := new Holder();
      h.R3(lambda() => 7);
      n := n + h.Fire();
      i := i + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
