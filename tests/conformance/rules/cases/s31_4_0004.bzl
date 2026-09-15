//! case: s31_4_0004
//! doc: spec
//! section: 31.4
//! title: ArgumentException.ParamName is null when not supplied
//! rule: `public readonly property ParamName: string?;   // null when not
//!   supplied` — the `(message)`-only constructor leaves it null.
//! cites: 31.4
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    try
      throw new ArgumentException("must be positive");
    catch
      var ex: ArgumentException => Console.WriteLine($"{ex.ParamName}"),
    end try;
  end Main;
end Program;
