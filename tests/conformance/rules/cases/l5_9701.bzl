//! case: l5_9701
//! doc: stdlib
//! section: 5
//! title: FirstOrNull, LastOrNull and SingleOrNull over an empty Dictionary are null
//! rule: non-throwing form: the `*OrNull` variants, which return `null` instead
//! cites: 5, 4.8
//! expect: run
//! output: True True True
//! notes: Pins #9234 (pick 93797636b): FirstOrNull, LastOrNull and SingleOrNull are the non-throwing forms
//!   of First, Last and Single over a Dictionary<K, V>, which is an ICollection<DictionaryEntry<K,
//!   V>> and so carries the 5 element-access members; the unfixed compiler returned the wrong value
//!   or lowered nothing. The empty source has no element, so each of the three returns null instead
//!   of raising; a null comparison renders True.
module Conf;

public class Program
  public static method Main()
    using var e := new Dictionary<string, int>();
    var a := e.FirstOrNull();
    var b := e.LastOrNull();
    var c := e.SingleOrNull();
    Console.WriteLine($"{a = null} {b = null} {c = null}");
  end Main;
end Program;
