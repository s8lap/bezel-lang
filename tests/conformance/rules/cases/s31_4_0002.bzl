//! case: s31_4_0002
//! doc: spec
//! section: 31.4
//! title: an ArgumentException arm catches an ArgumentNullException
//! rule: The inheritance hierarchy is relevant for arm ordering: an arm
//!   pattern of `ArgumentException` matches `ArgumentNullException`.
//! cites: 31.4
//! expect: run
//! stdout: bad arg
module Conf;

public class Program
  public static method Main()
    try
      throw new ArgumentNullException("value required");
    catch
      ArgumentException => Console.WriteLine("bad arg"),
    end try;
  end Main;
end Program;
