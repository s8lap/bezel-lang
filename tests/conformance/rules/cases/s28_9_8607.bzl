//! case: s28_9_8607
//! doc: spec
//! section: 28.9
//! title: a call through a function-typed field survives the field being cleared across an await
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b12_async). Run is async: it awaits Task.Delay(1), then calls through the field; the closure clears the field and returns k+1. Sum = 399. The call itself is made after the await, so no suspension happens while the closure runs.
module Conf;
import System;
import System.Threading;
public class Hx
  private _f: ((Hx) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _f := null; end Drop;
  public async method Run(): Task<int>
    await Task.Delay(1);
    if _f <> null then return _f!(this); end if;
    return 7;
  end Run;
end Hx;
public class Program
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3); t := t + await h.Run();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
