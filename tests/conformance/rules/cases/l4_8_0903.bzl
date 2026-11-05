//! case: l4_8_0903
//! doc: stdlib
//! section: 4.8
//! title: Dictionary.Add(entry) with an entry from iteration adds the pair, and Contains(entry) then finds it
//! rule: `Add(entry)` is `Add(entry.Key, entry.Value)` — `ArgumentException` on a present key ...
//!   `Contains(entry)` is `true` iff the key is present **and** the stored value equals
//!   `entry.Value` under the type's standard equality
//! expect: run
//! output: y
//!   1
//! issue: 8169
module Conf;

public class Program
  public static method Yn(b: bool): string
    if b then
      return "y";
    end if;
    return "n";
  end Yn;

  public static method Main()
    var src := new Dictionary<string, int>();
    src.Add("a", 1);
    var d := new Dictionary<string, int>();
    for var e in src do
      d.Add(e);
      Console.WriteLine(Program.Yn(d.Contains(e)));
    end for;
    Console.WriteLine(d.Count);
  end Main;
end Program;
