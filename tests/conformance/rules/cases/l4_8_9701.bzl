//! case: l4_8_9701
//! doc: stdlib
//! section: 4.8
//! title: FirstOrNull and LastOrNull over a three-entry Dictionary follow insertion order
//! rule: one per live entry in a single pass, in insertion order
//! cites: 4.8, 5
//! expect: run
//! output: first p 1
//!   last r 3
//! notes: Pins #9234 (pick 93797636b): FirstOrNull, LastOrNull and SingleOrNull are the non-throwing forms
//!   of First, Last and Single over a Dictionary<K, V>, which is an ICollection<DictionaryEntry<K,
//!   V>> and so carries the 5 element-access members; the unfixed compiler returned the wrong value
//!   or lowered nothing. Iteration over a Dictionary is in insertion order (stdlib 4.8), so the first
//!   element is p and the last is r. SingleOrNull over several entries is deliberately not asserted
//!   (the no-argument form's behaviour with more than one element is a spec question).
module Conf;

public class Program
  public static method Main()
    using var m := new Dictionary<string, int>();
    m["p"] := 1;
    m["q"] := 2;
    m["r"] := 3;
    var a := m.FirstOrNull();
    var b := m.LastOrNull();
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
  end Main;
end Program;
