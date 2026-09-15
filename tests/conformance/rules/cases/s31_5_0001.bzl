//! case: s31_5_0001
//! doc: spec
//! section: 31.5
//! title: throw with an operand may appear as the right operand of ??
//! rule: `throw` — with an operand, or the bare re-raise — may appear in any
//!   expression position, per §17.12's throw expression and its bottom type.
//! cites: 31.5, 17.12
//! expect: run
//! stdout: no config
module Conf;

public class Config
  public constructor()
  end constructor;
end Config;

public class Program
  public static method LoadConfig(): owned Config? => null;

  public static method Main()
    try
      var cfg := LoadConfig() ?? throw new InvalidOperationException("no config");
      Console.WriteLine("loaded");
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
