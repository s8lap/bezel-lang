//! case: s28_3_11_7903
//! doc: spec
//! section: 28.3.11
//! title: an owning-choice temporary passed to a callee that sometimes throws is disposed
//!   exactly once
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is
//!   produced, after every use of it within that expression has completed
//! cites: 28.3.11, 28.3.10
//! expect: run
//! output: 200
//!   100
module M;
import System;

public class Res
  public static N: int := 0;
  public constructor() end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;

public choice OChoice Text(owned r: Res), Empty;
end OChoice;

public class H
  public constructor() end constructor;
  public method Thr(owned o: OChoice, c: bool): OChoice
    if c then throw new InvalidOperationException("boom"); end;
    return o;
  end Thr;
end H;

public class Program
  public static method Main()
    var h := new H();
    var i := 0;
    var caught := 0;
    while i < 200 do
      try
        h.Thr(OChoice.Text(new Res()), i mod 2 = 0);
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine(Res.N);
    Console.WriteLine(caught);
  end Main;
end Program;
