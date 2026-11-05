//! case: l4_8_0904
//! doc: stdlib
//! section: 4.8
//! title: Dictionary.Add(entry) on a present key raises ArgumentException and leaves the stored value
//! rule: `Add(entry)` is `Add(entry.Key, entry.Value)` — `ArgumentException` on a present key
//! expect: run
//! output: AE
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
    var d := new Dictionary<string, int>();
    d.Add("a", 1);
    try
      d.Add(new DictionaryEntry<string, int>("a", 2));
    catch
      ArgumentException => Console.WriteLine("AE")
    end try;
    Console.WriteLine(d["a"]);
  end Main;
end Program;
