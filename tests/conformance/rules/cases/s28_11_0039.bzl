//! case: s28_11_0039
//! doc: spec
//! section: 28.11
//! title: move with at an absent dictionary key raises, inserts nothing and disposes the replacement
//! rule: `move d[k] with fresh` at an absent key raises
//!   `KeyNotFoundException` and inserts nothing, as the indexer-get does
//! cites: 28.11, 28.8.1, 28.3
//! expect: run
//! output: missing
//!   1 1 False
//! issue: 5923
module Conf;

public class Res
  public static Disposed: int := 0;
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Res.Disposed += 1;
  end destructor;
end Res;

public class Program
  public static method Main()
    using var d := new OwningDictionary<string, Res>();
    var absent := "absent";
    d.Add("a", new Res("a"));
    try
      using var t := move d[absent] with new Res("fresh");
      Console.WriteLine("exchanged");
    catch
      KeyNotFoundException => Console.WriteLine("missing")
    end;
    Console.WriteLine($"{Res.Disposed} {d.Count} {d.ContainsKey(absent)}");
  end Main;
end Program;
