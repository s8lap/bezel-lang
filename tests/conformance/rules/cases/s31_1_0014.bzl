//! case: s31_1_0014
//! doc: spec
//! section: 31.1
//! title: a generic class whose type parameter is bounded by Exception throws an owned argument
//! rule: "the operand's static type, nullability not admitted, must be"
//! cites: 31.1, 14.2
//! expect: run
//! output: gt
module Conf;

public class Thrower<T> where T: Exception
  public method Go(owned x: T)
    throw x;
  end Go;
end Thrower;

public class Program
  public static method Main()
    var t := new Thrower<ArgumentException>();
    try
      t.Go(new ArgumentException("gt"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
