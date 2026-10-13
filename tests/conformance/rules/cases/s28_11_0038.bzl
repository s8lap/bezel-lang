//! case: s28_11_0038
//! doc: spec
//! section: 28.11
//! title: move with on an OwningDictionary entry yields the old value and the entry holds the replacement
//! rule: `move coll[i] with e` is the replacing `move` at an owning slot: it
//!   stores `e` into the i-th slot and yields the slot's previous element,
//!   owned, in one step
//! cites: 28.11, 28.8.1
//! expect: run
//! stdout: a b 1
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Program
  public static method Main()
    using var d := new OwningDictionary<string, Res>();
    var key := "k1";
    d.Add(key, new Res("a"));
    using var t := move d[key] with new Res("b");
    Console.WriteLine($"{t.Name} {d[key].Name} {d.Count}");
  end Main;
end Program;
