//! case: s27_3_0001
//! doc: spec
//! section: 27.3
//! title: a lambda's parameter types are inferred from the target type
//! rule: When the lambda's target type is known from context, parameter type
//!   annotations may be omitted; the compiler infers them from the target.
//! cites: 27.3, 27.4
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    var twice: (int) -> int := lambda(x) => x * 2;
    Console.WriteLine($"{twice(21)}");
  end Main;
end Program;
