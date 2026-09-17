//! case: s31_1_0010
//! doc: spec
//! section: 31.1
//! title: a closure over the caught exception assigned outside the arm is rejected
//! rule: A closure capturing the catch-arm binding may outlive the arm only
//!   when the closure's value can leave the arm; assigning it to a binding
//!   declared outside the arm is one such escape, and the counter-example is
//!   one assignment away from the confined-closure idiom.
//! cites: 31.1, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Program
  public static method Use(ex: Exception): int => ex.Message.Length;

  public static method Main()
    var outer: () -> int := lambda() => 0;
    try
      throw new ArgumentException("boom");
    catch
      var ex: Exception => do
        var mk := lambda(): () -> int => do return lambda() => Use(ex); end;
        outer := mk();
      end,
    end try;
    Console.WriteLine(outer());
  end Main;
end Program;
