//! case: l4_8_0908
//! doc: stdlib
//! section: 4.8
//! title: Dictionary.Contains(entry) compares string values by content, not identity
//! rule: `Contains(entry)` is `true` iff the key is present **and** the stored value equals
//!   `entry.Value` under the type's standard equality
//! expect: run
//! output: y
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
    var i := 7;
    var d := new Dictionary<string, string>();
    d.Add("k", $"v{i}");
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<string, string>("k", $"v{i}"))));
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<string, string>("k", $"v{i + 1}"))));
  end Main;
end Program;
