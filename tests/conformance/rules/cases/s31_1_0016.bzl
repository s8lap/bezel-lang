//! case: s31_1_0016
//! doc: spec
//! section: 31.1
//! title: a throw of a local copied from an owned parameter without move is rejected as borrowed
//! rule: "an unmarked read of an owned binding is a borrow of the binding."
//! cites: 31.1, 28.7
//! expect: reject
//! diagnostic: must be an OWNED exception
module Conf;

public class Program
  public static method M<T>(owned x: T) where T: Exception
    var y := x;
    throw y;
  end M;

  public static method Main()
    try
      M(new ArgumentException("wrapped"));
    catch
      var e: ArgumentException => Console.WriteLine($"AE {e.Message}")
    end try;
  end Main;
end Program;
