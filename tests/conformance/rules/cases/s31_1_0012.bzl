//! case: s31_1_0012
//! doc: spec
//! section: 31.1
//! title: a throw of an owned parameter bounded by Exception is caught by its catch arm
//! rule: "the operand's static type, nullability not admitted, must be"
//! cites: 31.1, 14.2
//! expect: run
//! output: m
module Conf;

public class Program
  public static method M<T>(owned x: T) where T: Exception
    throw x;
  end M;

  public static method Main()
    try
      M(new IOException("m"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
