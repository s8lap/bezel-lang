//! case: s8_5_0010
//! doc: spec
//! section: 8.5
//! title: a null-conditional read of a function-typed field renders null or the function type
//! rule: For function values the ToString rendering is the stdlib 2.13 FullName of the
//!   value's own function type; a null hole (here the short-circuited ?. result)
//!   renders null (spec 8.5, 18.5).
//! cites: 18.5, 7.5
//! expect: run
//! output: null/(int) -> int
module Conf;

public class Holder(c: ((int) -> int)?)
  public Cb: ((int) -> int)? := c;
end Holder;

public class Program
  public static method Main()
    var k := 2;
    var h := new Holder(lambda(x: int) => x + k);
    var hn: Holder? := null;
    var hs: Holder? := h;
    Console.WriteLine($"{hn?.Cb}/{hs?.Cb}");
  end Main;
end Program;
