//! case: s5_1_0142
//! doc: spec
//! section: 5.1
//! title: a constant long index narrows to an int index parameter by value
//! rule: The rule is value-based, not type-based: the source may be a
//!   compile-time constant of any integer type — not only `int` — and the
//!   target any integer type whose range contains its value, across
//!   signedness.
//! cites: 5.1, 17.14
//! expect: run
//! stdout: 20
module Conf;

public class Program
  public static method Main(): int
    var a: Array<int> := [10, 20, 30];
    const N: long := 1;
    Console.WriteLine(a[N]);
    return 0;
  end Main;
end Program;
