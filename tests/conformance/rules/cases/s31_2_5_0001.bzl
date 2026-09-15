//! case: s31_2_5_0001
//! doc: spec
//! section: 31.2.5
//! title: a catch-all arm whose body is the unit value suppresses the exception
//! rule: To deliberately suppress all catchable exceptions in a block, use a
//!   bare-`Exception` arm whose body is the unit value `()`; the `()` body
//!   makes the suppression syntactically explicit.
//! cites: 31.2.5
//! expect: run
//! stdout: continued
module Conf;

public class Program
  public static method Main()
    try
      throw new ArgumentException("boom");
    catch
      Exception => (),
    end try;
    Console.WriteLine("continued");
  end Main;
end Program;
