//! case: l4_8_0907
//! doc: stdlib
//! section: 4.8
//! title: Dictionary.Contains(entry) compares a record value under the record's standard equality
//! rule: `Contains(entry)` is `true` iff the key is present **and** the stored value equals
//!   `entry.Value` under the type's standard equality
//! expect: run
//! output: y
//!   n
//! issue: 8169
module Conf;

public record Pt(X: int, Y: int);

public class Program
  public static method Yn(b: bool): string
    if b then
      return "y";
    end if;
    return "n";
  end Yn;

  public static method Main()
    var d := new Dictionary<int, Pt>();
    d.Add(1, new Pt(3, 4));
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<int, Pt>(1, new Pt(3, 4)))));
    Console.WriteLine(Program.Yn(d.Contains(new DictionaryEntry<int, Pt>(1, new Pt(3, 5)))));
  end Main;
end Program;
