//! case: s28_9_8601
//! doc: spec
//! section: 28.9
//! title: a call through a function-typed field survives the closure replacing that field mid-call
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b2_reassign). Fill stores a closure that, while running, calls Swap, which overwrites the field with a second closure; the first closure then reads its captured k. The function value lives until the call through it returns, so k+1 is read intact: the sum over i in 0..199 of (i mod 3)+1 is 67*1+67*2+66*3 = 399. A 200-iteration loop makes a read of freed closure memory visible.
module Conf;
import System;
public class Hx
  private _f: ((Hx) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Swap(k); return k + 1; end; end Fill;
  public method Swap(k: int) _f := lambda(x: Hx) => k + 5; end Swap;
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
