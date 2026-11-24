//! case: s28_3_8_8602
//! doc: spec
//! section: 28.3.8
//! title: a generic ref swap that moves a call-bounded function value into a local is rejected at the call
//! rule: A `ref` argument's current value at a location declared at a bare type parameter is still checked there as at a set-erasing position
//! cites: 28.3.8
//! expect: reject
//! diagnostic: set-erasing
//! notes: Harvested from the #9075 review (probe a14b; the accepting twin is s28_3_8_8601). The
//!   local g holds the plain function-typed parameter q, whose set is non-empty (a call-bounded
//!   callback). SwapG's `ref b: T` is declared at a bare type parameter and the call substitutes
//!   a function type for it, so g's current value is checked there as at a set-erasing position
//!   and rejected. The only change from s28_3_8_8601 is where g's value comes from.
module Conf;
import System;
public class Program
  public static method SwapG<T>(ref a: T, ref b: T)
    var t: T := a;
    a := b;
    b := t;
  end SwapG;
  public static method Via(q: () -> int): int
    var f: () -> int := lambda(): int => 9;
    var g: () -> int := q;
    Program.SwapG(ref f, ref g);
    return f();
  end Via;
  public static method Main(): int
    var c := 40;
    Console.WriteLine(Program.Via(lambda(): int => c));
    return 0;
  end Main;
end Program;
