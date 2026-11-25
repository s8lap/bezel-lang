//! case: s28_9_9004
//! doc: spec
//! section: 28.9
//! title: the same call where the interface and its implementing class come from an imported module
//! rule: A property or indexer read is a lend for every declared type — the A4/§28.6 rule for class and
//!   interface types, and the same face for runtime-managed types: a getter never transfers a reclamation
//!   obligation to its reader
//! cites: 27.5, 28.3
//! expect: run
//! output: 399
//! notes: Harvested from the #9147 review (mod/app2: interface AND class in the library module). Trace: the
//!   closure installed by `Fill(k)` calls `x.Drop()` (which replaces the property) and THEN reads its
//!   captured `k` and returns k + 1, so the running closure must stay alive across the property
//!   replacement. k = i mod 3 over i = 0..199: 66 full cycles of (1+2+3) = 396, plus i = 198 (1) and i =
//!   199 (2) = 399. Value-and-no-crash only; the corpus has no sanitizer leg, so the UAF the fix removed
//!   may be silent on the parent.
module Conf;
import Lib2;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3);
      var ih: IHx := h;
      t := t + ih.P(ih);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
