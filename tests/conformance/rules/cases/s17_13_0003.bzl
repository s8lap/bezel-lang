//! case: s17_13_0003
//! doc: spec
//! section: 17.13
//! title: ref _ is a compile error
//! rule: "The discard composes with `out` only: **`ref _` is a compile
//!   error** — a `ref` argument reads its operand, which a discard cannot
//!   supply; pass a named location."
//! cites: 17.13, 22.3
//! expect: reject
//! diagnostic: ref
module Conf;

public class Program
  public static method Swap(ref a: int, ref b: int)
    var t := a;
    a := b;
    b := t;
  end Swap;

  public static method Main()
    var x := 1;
    Swap(ref x, ref _);
  end Main;
end Program;
