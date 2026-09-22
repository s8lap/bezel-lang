//! case: s31_1_0009
//! doc: spec
//! section: 31.1
//! title: disposing the caught exception binding is rejected
//! rule: Inside a catch arm, the bound exception variable is a borrow of the
//!   in-flight exception; `Dispose()` requires an owning receiver, and a
//!   catch-arm binding is one of the borrow forms through which disposal is a
//!   compile error with no exception.
//! cites: 31.1, 28.6
//! expect: reject
//! diagnostic: borrow
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message);
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      throw new BoomException("boom");
    catch
      var ex: BoomException => ex.Dispose(),
    end try;
  end Main;
end Program;
