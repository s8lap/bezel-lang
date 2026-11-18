//! case: s7_5_7114
//! doc: spec
//! section: 7.5
//! title: `?.FirstKey ?? e` stored to a field and pushed on a collection keeps the key a later read sees
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: 82200
//!   none
module Conf;
public class Holder
  public Name: string;
  public constructor()
    Name := "init";
  end constructor;
end Holder;
public class Program
  public static method Run(d: SortedDictionary<string, int>?, h: Holder, l: Stack<string>): int
    h.Name := d?.FirstKey ?? "none";
    l.Push(d?.LastKey ?? "none");
    return h.Name.Length + l.Count;
  end Run;
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    d.Add("pear" + "x", 1);
    d.Add("apple" + "y", 2);
    using var h := new Holder();
    using var l := new Stack<string>();
    var sum := 0;
    for var i in 0..<200 do
      sum := sum + Run(d, h, l);
      sum := sum + Run(null, h, l);
    end;
    Console.WriteLine(sum);
    Console.WriteLine(h.Name);
  end Main;
end Program;
