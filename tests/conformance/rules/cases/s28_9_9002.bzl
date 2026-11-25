//! case: s28_9_9002
//! doc: spec
//! section: 28.9
//! title: the same call from an async body, after an await, survives the property being replaced mid-call
//! rule: A property or indexer read is a lend for every declared type — the A4/§28.6 rule for class and
//!   interface types, and the same face for runtime-managed types: a getter never transfers a reclamation
//!   obligation to its reader
//! cites: 27.5, 28.3
//! expect: run
//! output: 399
//! notes: Harvested from the #9147 review (f_async). Trace: the closure installed by `Fill(k)` calls
//!   `x.Drop()` (which replaces the property) and THEN reads its captured `k` and returns k + 1, so the
//!   running closure must stay alive across the property replacement. k = i mod 3 over i = 0..199: 66
//!   full cycles of (1+2+3) = 396, plus i = 198 (1) and i = 199 (2) = 399. Value-and-no-crash only; the
//!   corpus has no sanitizer leg, so the UAF the fix removed may be silent on the parent.
module Conf;
public interface IHx
  property P: (IHx) -> int;
  method Drop();
end IHx;
public class Hx: IHx
  public property P: (IHx) -> int := lambda(x: IHx) => 0;
  public constructor() end constructor;
  public method Drop() P := lambda(x: IHx) => 0; end Drop;
  public method Fill(k: int) P := lambda(x: IHx) => do x.Drop(); return k + 1; end; end Fill;
end Hx;
public class Program
  public static async method Run(ih: IHx): Task<int>
    await Task.Delay(0);
    return ih.P(ih);
  end Run;
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3);
      var ih: IHx := h;
      t := t + await Run(ih);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
