//! case: s17_12_0002
//! doc: spec
//! section: 17.12
//! title: bare throw re-raises the currently-handled exception
//! rule: "`throw` (no operand) re-raises the currently-handled exception with
//!   its stack trace preserved. Valid only as an expression appearing inside
//!   a catch arm body."
//! cites: 17.12, 31.2.4
//! expect: run
//! stdout: boom
module Conf;

public class Program
  public static method Main()
    try
      try
        throw new InvalidOperationException("boom");
      catch
        var inner: Exception => throw
      end try;
    catch
      var outer: Exception => Console.WriteLine(outer.Message)
    end try;
  end Main;
end Program;
