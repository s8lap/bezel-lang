//! case: l1_0006
//! doc: stdlib
//! section: 1
//! title: Try shape (c) assigns the parsed value on the true path
//! rule: Try shape (c) assigns the real, non-default value to the out parameter
//!   when the operation succeeds; Convert.TryToInt32 returns true and the parsed
//!   integer on well-formed input.
//! cites: 2.3
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    var ok := Convert.TryToInt32("42", out var result);
    Console.WriteLine(result);
  end Main;
end Program;
