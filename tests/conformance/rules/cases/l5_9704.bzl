//! case: l5_9704
//! doc: stdlib
//! section: 5
//! title: the null-conditional read of FirstOrNull over a Dictionary falls back to the coalesced default
//! rule: non-throwing form: the `*OrNull` variants, which return `null` instead
//! cites: 5, 4.8, 7.5
//! expect: run
//! output: 1 -1
//! notes: Pins #9234 (pick 93797636b): FirstOrNull, LastOrNull and SingleOrNull are the non-throwing forms
//!   of First, Last and Single over a Dictionary<K, V>, which is an ICollection<DictionaryEntry<K,
//!   V>> and so carries the 5 element-access members; the unfixed compiler returned the wrong value
//!   or lowered nothing. m has the entry p: 1 so v is 1; e is empty so FirstOrNull() is null,
//!   `?.Value` is null and the coalesce gives -1.
module Conf;

public class Program
  public static method Main()
    using var m := new Dictionary<string, int>();
    m["p"] := 1;
    using var e := new Dictionary<string, int>();
    var v := m.FirstOrNull()?.Value ?? -1;
    var w := e.FirstOrNull()?.Value ?? -1;
    Console.WriteLine($"{v} {w}");
  end Main;
end Program;
