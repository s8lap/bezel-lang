//! case: l4_8_0906
//! doc: stdlib
//! section: 4.8
//! title: Dictionary.Remove(entry) removes only when key and value both match
//! rule: `Remove(entry)` removes only when both match, else returns `false` with no change
//! expect: run
//! output: n
//!   1
//!   y
//!   0
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
    Console.WriteLine(Program.Yn(d.Remove(new DictionaryEntry<string, int>("a", 2))));
    Console.WriteLine(d.Count);
    Console.WriteLine(Program.Yn(d.Remove(new DictionaryEntry<string, int>("a", 1))));
    Console.WriteLine(d.Count);
    Console.WriteLine(Program.Yn(d.Remove(new DictionaryEntry<string, int>("a", 1))));
  end Main;
end Program;
