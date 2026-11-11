//! case: l6_8_4301
//! doc: stdlib
//! section: 6.8
//! title: FaultStackTrace is readable as a string on a DomainFaultedException caught at a join
//! rule: public readonly property FaultStackTrace: string;
//! cites: 6.8, 30.14
//! expect: run
//! output: True
//!   boom
//! issue: 8399
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
      var ex: DomainFaultedException => do
        var st: string := ex.FaultStackTrace;
        Console.WriteLine(st.Length >= 0);
        Console.WriteLine(ex.Message);
      end,
    end try;
  end Main;
end Program;
