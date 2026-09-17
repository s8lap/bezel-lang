//! case: s18_5_0008
//! doc: spec
//! section: 18.5
//! title: a format spec dispatches to ToString(format); a user type opts in via IFormattable
//! rule: "A hole with a format spec dispatches to `ToString(format: string)`: ...
//!   a user type opts in by implementing the auto-imported interface
//!   `IFormattable`."
//! cites: 18.5
//! expect: run
//! stdout: fmt:X
module Conf;

public class Tag: IFormattable
  public constructor()
  end constructor;

  public override method ToString(): string => "plain";
  public method ToString(format: string): string => $"fmt:{format}";
end Tag;

public class Program
  public static method Main()
    var t := new Tag();
    Console.WriteLine($"{t:X}");
  end Main;
end Program;
