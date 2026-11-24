//! case: s28_3_8_8604
//! doc: spec
//! section: 28.3.8
//! title: a string ref beside a ref holding a call-bounded function value is admitted at an Object parameter
//! rule: A `ref` or plain `out` argument location ([§22.3](#sec-ref-and-out-argument-locations)) whose type, after substitution, can carry a borrow — a class or interface type, a function type, or a borrow-bearing record, choice or inline array
//! cites: 28.3.8, 22.3
//! expect: run
//! stdout: xy
//! notes: Harvested from the #9075 review (probe a30; the string twin of s28_3_8_8603). A string is
//!   a runtime-managed type, not a class, interface, function type, record, choice or inline
//!   array, so its `ref` location cannot carry a borrow and receives none of the call's sources.
//!   MixS appends "y" to "x".
module Conf;
import System;
public class Program
  public static method MixS(ref s: string, ref a: () -> int)
    s := s + "y";
  end MixS;
  public static method Show(o: Object): string
    return o.ToString();
  end Show;
  public static method Via(q: () -> int): string
    var s: string := "x";
    var g: () -> int := q;
    Program.MixS(ref s, ref g);
    return Program.Show(s);
  end Via;
  public static method Main(): int
    var c := 40;
    Console.WriteLine(Program.Via(lambda(): int => c));
    return 0;
  end Main;
end Program;
