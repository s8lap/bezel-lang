//! case: l3_3_0005
//! doc: stdlib
//! section: 3.3
//! title: a zero-length receiver never contains a non-empty needle
//! rule: A zero-length receiver is well-defined: every non-empty needle is
//!   not found — Contains returns false.
//! cites: 3.3
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var span := "".AsSpan();
    Console.WriteLine(span.Contains("x"));
  end Main;
end Program;
