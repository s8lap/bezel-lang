//! case: s35_4_0019
//! doc: spec
//! section: 35.4
//! title: a generic choice is classified per instantiation: C<DateTime> over a value type is value-like
//! rule: Otherwise the choice is **value-like**: every payload is a value, a `string`, a
//!   runtime-managed type, or (transitively) a value-like record or choice.
//! cites: 35.4, 35.7
//! expect: run
//! stdout: True
module Conf;

public choice C<DateTime> At(p: DateTime), Never; end C;

public class Program
  public static method Main()
    var x := C<int>.At(1);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
