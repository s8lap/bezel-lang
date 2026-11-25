//! case: l5_9702
//! doc: stdlib
//! section: 5
//! title: FirstOrNull, LastOrNull and SingleOrNull over a one-entry Dictionary are that entry
//! rule: non-throwing form: the `*OrNull` variants, which return `null` instead
//! cites: 5, 4.8
//! expect: run
//! output: first x 5
//!   last x 5
//!   single x 5
//! notes: Pins #9234 (pick 93797636b): FirstOrNull, LastOrNull and SingleOrNull are the non-throwing forms
//!   of First, Last and Single over a Dictionary<K, V>, which is an ICollection<DictionaryEntry<K,
//!   V>> and so carries the 5 element-access members; the unfixed compiler returned the wrong value
//!   or lowered nothing. With exactly one entry x: 5 the first, last and sole element are that entry.
module Conf;

public class Program
  public static method Main()
    using var o := new Dictionary<string, int>();
    o["x"] := 5;
    var a := o.FirstOrNull();
    var b := o.LastOrNull();
    var c := o.SingleOrNull();
    if a = null then
      Console.WriteLine("first null");
    else
      Console.WriteLine($"first {a.Key} {a.Value}");
    end if;
    if b = null then
      Console.WriteLine("last null");
    else
      Console.WriteLine($"last {b.Key} {b.Value}");
    end if;
    if c = null then
      Console.WriteLine("single null");
    else
      Console.WriteLine($"single {c.Key} {c.Value}");
    end if;
  end Main;
end Program;
