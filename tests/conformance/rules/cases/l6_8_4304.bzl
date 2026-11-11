//! case: l6_8_4304
//! doc: stdlib
//! section: 6.8
//! title: FaultStackTrace is not a member of Exception and a read through an Exception arm is rejected
//! rule: The exception types that carry declared surface beyond `Exception`'s are enumerated by reference rather than counted
//! cites: 6.8, 31.4
//! expect: reject
//! diagnostic: no member `FaultStackTrace` on `Exception`
module Conf;

import System.Threading;

public class Program
  public static method Explode(): int
    throw new InvalidOperationException("boom");
  end Explode;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Explode);
    try
      var n := await t;
    catch
      var ex: Exception => Console.WriteLine(ex.FaultStackTrace),
    end try;
  end Main;
end Program;
