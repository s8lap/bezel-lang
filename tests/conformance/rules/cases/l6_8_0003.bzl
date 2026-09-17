//! case: l6_8_0003
//! doc: stdlib
//! section: 6.8
//! title: a fault in the entry surfaces at the join as DomainFaultedException
//! rule: DomainFaultedException ... Message carries the original
//!   exception's message text.
//! cites: 6.8, 30.14
//! expect: run
//! stdout: boom
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
      var ex: DomainFaultedException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
