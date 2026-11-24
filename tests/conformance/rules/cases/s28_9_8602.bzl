//! case: s28_9_8602
//! doc: spec
//! section: 28.9
//! title: a call through a function-typed field survives the closure clearing that field mid-recursion
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 996
//! notes: Harvested from the #9096 review (probe b5_recur). The closure recurses through Step three levels and at depth 3 calls Drop, which nulls the field while every outer frame is still inside the same closure and later adds k. Each run yields (k+1)+3k = 4k+1; summing over i in 0..199 gives 4*199+200 = 996 (199 = 67*0+67*1+66*2).
module Conf;
import System;
public class Hx
  private _f: ((Hx, int) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx, d: int) => do if d >= 3 then x.Drop(); return k + 1; end if; return x.Step(d + 1) + k; end; end Fill;
  public method Drop() _f := null; end Drop;
  public method Step(d: int): int
    if _f <> null then return _f!(this, d); end if;
    return 0;
  end Step;
  public method Run(): int
    return Step(0);
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
