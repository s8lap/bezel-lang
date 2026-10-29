//! case: s8_5_0009
//! doc: spec
//! section: 8.5
//! title: a function-typed field read renders its function type or null at a hole
//! rule: For function values the ToString rendering is the stdlib 2.13 FullName of the
//!   value's own function type, "(int) -> int"; a null hole renders null (spec 8.5).
//! cites: 18.5, 27.1
//! expect: run
//! output: (int) -> int/null
module Conf;

public class Holder(c: ((int) -> int)?)
  public Cb: ((int) -> int)? := c;
end Holder;

public class Program
  public static method Main()
    var k := 5;
    var a: ((int) -> int)? := lambda(x: int) => x + k;
    var h := new Holder(a);
    var h0 := new Holder(null);
    Console.WriteLine($"{h.Cb}/{h0.Cb}");
  end Main;
end Program;
