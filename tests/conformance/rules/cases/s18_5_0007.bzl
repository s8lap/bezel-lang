//! case: s18_5_0007
//! doc: spec
//! section: 18.5
//! title: a hole without a format spec dispatches to the value's ordinary ToString()
//! rule: "A hole without a format spec formats via the value's ordinary
//!   `ToString()` (§8.5)."
//! cites: 18.5, 8.5
//! expect: run
//! stdout: plain
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
    Console.WriteLine($"{t}");
  end Main;
end Program;
