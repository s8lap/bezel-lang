//! case: s31_1_0013
//! doc: spec
//! section: 31.1
//! title: a throw of an owned parameter bounded by a user Exception subclass is caught
//! rule: "the operand's static type, nullability not admitted, must be"
//! cites: 31.1, 14.2
//! expect: run
//! output: m2
module Conf;

public class AppError: Exception
  public constructor(m: string) base(m); end constructor;
end AppError;

public class Program
  public static method M<T>(owned x: T) where T: AppError
    throw x;
  end M;

  public static method Main()
    try
      M(new AppError("m2"));
    catch
      var e: Exception => Console.WriteLine(e.Message)
    end try;
  end Main;
end Program;
