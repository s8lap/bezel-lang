//! case: s28_9_8606
//! doc: spec
//! section: 28.9
//! title: a call statement through a function-typed field survives the closure clearing the field
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b8_stmt). The call is an expression statement; the closure clears the field and then stores k+1 into _n, which Run returns. Sum over i of (i mod 3)+1 = 399.
module Conf;
import System;
public class Hx
  public _n: int := 0;
  private _f: ((Hx) -> ())? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); x._n := k + 1; end; end Fill;
  public method Drop() _f := null; end Drop;
  public method Run(): int
    if _f <> null then _f!(this); end if;
    return _n;
  end Run;
end Hx;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
