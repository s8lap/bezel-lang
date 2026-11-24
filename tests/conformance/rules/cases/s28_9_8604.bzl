//! case: s28_9_8604
//! doc: spec
//! section: 28.9
//! title: a call through a function-typed field of a generic class survives the closure clearing the field
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b6_generic). Same shape as s28_9_8601 in a generic class Gx<T> instantiated at string: the closure calls Drop (field := null) and then returns k+1. Sum = 399.
module Conf;
import System;
public class Gx<T>
  private _f: ((Gx<T>) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Gx<T>) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _f := null; end Drop;
  public method Run(): int
    if _f <> null then return _f!(this); end if;
    return 7;
  end Run;
end Gx;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Gx<string>(); h.Fill(i mod 3); t := t + h.Run();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
