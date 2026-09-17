//! case: s7_5_0003
//! doc: spec
//! section: 7.5
//! title: a null segment anywhere in a chain makes the whole chain null
//! rule: If any segment of a chain is null, the remainder of the chain is
//!   skipped on the same terms and the whole chain's result is `null`.
//! expect: run
//! stdout: no length
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string? := null;
end Widget;

public class Program
  public static method Main()
    var w: Widget? := new Widget();
    var len: int? := w?.Name?.Length;
    if len = null then
      Console.WriteLine("no length");
    else
      Console.WriteLine("has length");
    end;
  end Main;
end Program;
