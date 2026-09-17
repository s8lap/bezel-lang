//! case: s31_4_0003
//! doc: spec
//! section: 31.4
//! title: ArgumentException.ParamName holds the supplied parameter name
//! rule: `ArgumentException` exposes `public readonly property ParamName:
//!   string?;` — the `(message, paramName)` constructor supplies it.
//! cites: 31.4
//! expect: run
//! stdout: amount
module Conf;

public class Program
  public static method Main()
    try
      throw new ArgumentException("must be positive", "amount");
    catch
      var ex: ArgumentException => Console.WriteLine($"{ex.ParamName}"),
    end try;
  end Main;
end Program;
