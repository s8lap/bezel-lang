//! case: s5_1_0148
//! doc: spec
//! section: 5.1
//! title: a uint constant is admitted at an int parameter by value
//! rule: spec §5.1: "An expression of integer type that is a **compile-time
//!     constant** (§17.14) implicitly converts to any integer type whose range
//!     contains its value ... The rule is **value-based, not type-based**: the
//!     source may be a compile-time constant of **any** integer type — not only
//!     `int` — and the target any integer type whose range contains the value,
//!     **across signedness**". The `uint` constant 16 has a value that fits
//!     `int`, so the radix parameter admits it.
//! cites: 5.1, 17.14
//! expect: run
//! stdout: 255
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", 16U));
  end Main;
end Program;
