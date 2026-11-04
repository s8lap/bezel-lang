//! case: s35_4_0023
//! doc: spec
//! section: 35.4
//! title: a generic choice with a type parameter named DateTime at int is value-like and compares with =
//! rule: A value-like choice is an ordinary copyable value type.
//! cites: 35.4, 35.7, 2.3
//! expect: run
//! issue: 7845
//! stdout: True

module Conf;

public choice C<DateTime>
  At(p: DateTime),
  Never;
end C;

public class Program
  public static method Main()
    var x := C<int>.At(1);
    var y := x;
    Console.WriteLine(x = y);
  end Main;
end Program;
