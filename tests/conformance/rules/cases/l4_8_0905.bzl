//! case: l4_8_0905
//! doc: stdlib
//! section: 4.8
//! title: Dictionary.Contains(entry) needs the key present and the stored value equal
//! rule: `Contains(entry)` is `true` iff the key is present **and** the stored value equals
//!   `entry.Value` under the type's standard equality
//! expect: run
//! output: y
//!   n
//!   n
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
    var d := new Dictionary<string, int>();
    d.Add("a", 1);
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<string, int>("a", 1))));
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<string, int>("a", 2))));
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<string, int>("b", 1))));
  end Main;
end Program;
