//! case: s14_5_0021
//! doc: spec
//! section: 14.5
//! title: under an explicit byte list a constant int argument whose value fits narrows by value
//! rule: With an explicit list — Pick<byte>(b, 250) — the parameter type is byte
//!   and the literal constant-narrows at the argument conversion site as usual.
//!   An expression of integer type that is a compile-time constant (§17.14)
//!   implicitly converts to any integer type whose range contains its value.
//! cites: 14.5, 5.1
//! expect: run
//! stdout: 200
module Conf;

public class Program
  public static method Main()
    const i: int := 200;
    Console.WriteLine(Math.Max<byte>(i, 3));
  end Main;
end Program;
