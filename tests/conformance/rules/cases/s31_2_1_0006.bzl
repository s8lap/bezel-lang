//! case: s31_2_1_0006
//! doc: spec
//! section: 31.2.1
//! title: a catch arm naming Object is rejected
//! rule: `Object` itself is rejected by the same rule for the opposite
//!   reason — as the universal root it matches every throwable, and the
//!   spelling for that is the bare `Exception` arm; the remedy is to delete
//!   the arm or fix the type.
//! cites: 31.2.1, 31.2.3
//! expect: reject
//! diagnostic: object
module Conf;

public class Program
  public static method DoWork()
  end DoWork;

  public static method Main()
    try
      DoWork();
    catch
      Object => Console.WriteLine("unreachable"),
    end try;
  end Main;
end Program;
