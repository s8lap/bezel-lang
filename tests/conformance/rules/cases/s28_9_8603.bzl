//! case: s28_9_8603
//! doc: spec
//! section: 28.9
//! title: a call through a field of an owned member object survives the closure replacing its own field
//! rule: Runtime management governs the *function value itself* — the heap-allocated closure object that backs it —
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 399
//! notes: Harvested from the #9096 review (probe b7_chain). The call B.F(B) goes through a function-typed field of an owned field; the closure calls Drop, which overwrites F, then returns k+1. Sum over i in 0..199 of (i mod 3)+1 = 399.
module Conf;
import System;
public class Inner
  public F: (Inner) -> int := lambda(x: Inner) => 0;
  public constructor() end constructor;
  public method Fill(k: int) F := lambda(x: Inner) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() F := lambda(x: Inner) => 0; end Drop;
end Inner;
public class Hx
  public owned B: Inner := new Inner();
  public constructor() end constructor;
  public method Fill(k: int) B.Fill(k); end Fill;
  public method Run(): int
    return B.F(B);
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
