//! case: s7_6_0001
//! doc: spec
//! section: 7.6
//! title: a null-conditional call on a null function value is a no-op
//! rule: A function value of nullable function type may be invoked with
//!   `?(...)`; the invocation is a no-op when the function value is null.
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method Main()
    var handler: ((int) -> ())? := null;
    handler?(42);
    Console.WriteLine("done");
  end Main;
end Program;
