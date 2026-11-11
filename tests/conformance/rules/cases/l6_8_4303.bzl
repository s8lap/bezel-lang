//! case: l6_8_4303
//! doc: stdlib
//! section: 6.8
//! title: FaultStackTrace read through a null-conditional on a non-null DomainFaultedException is non-null
//! rule: public readonly property FaultStackTrace: string;
//! cites: 6.8, 7.5
//! expect: run
//! stdout: False
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
        var m: DomainFaultedException? := ex;
        var s := m?.FaultStackTrace;
        Console.WriteLine(s is null);
      end,
    end try;
  end Main;
end Program;
