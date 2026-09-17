//! case: s27_3_0006
//! doc: spec
//! section: 27.3
//! title: a lambda literal at a target-typed non-call position admits an implicit widening body
//! rule: A lambda literal at a position that supplies a function-typed target
//!   other than a call argument is admitted under the argument-position rule
//!   (v0.292); its inferred return type must equal the target's return slot or
//!   implicitly convert to it — `var f: (int) -> long := lambda(x) => x * 2;`
//!   is admitted, the `int` body widening to `long`.
//! cites: 27.3, 5.1
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    var f: (int) -> long := lambda(x) => x * 2;
    Console.WriteLine($"{f(21)}");
  end Main;
end Program;
