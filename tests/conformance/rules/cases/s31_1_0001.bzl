//! case: s31_1_0001
//! doc: spec
//! section: 31.1
//! title: a thrown owned exception is caught by a matching arm
//! rule: `throw expr` constructs and raises an exception; a thrown exception is
//!   caught by the first arm naming its class or a base of it.
//! cites: 31.1, 31.2, 31.2.1
//! expect: run
//! stdout: bad amount
module Conf;

public class BadAmountException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BadAmountException;

public class Program
  public static method Main()
    try
      throw new BadAmountException("bad amount");
    catch
      var ex: BadAmountException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
