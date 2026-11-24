//! case: s28_9_8608
//! doc: spec
//! section: 28.9
//! title: a closure that re-stores the field's own value during the call leaves the call intact
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b3_self, the control where base and tip are both clean). Same() assigns _f := _f; the closure then returns k+1. Sum = 399.
module Conf;
import System;
public class Hx
  private _f: ((Hx) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Same(); return k + 1; end; end Fill;
  public method Same() _f := _f; end Same;
  public method Run(): int
    if _f <> null then return _f!(this); end if;
    return 7;
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
