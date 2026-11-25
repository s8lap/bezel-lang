//! case: l5_9705
//! doc: stdlib
//! section: 5
//! title: FirstOrNull, LastOrNull and SingleOrNull over fresh Dictionaries in a 200x loop
//! rule: non-throwing form: the `*OrNull` variants, which return `null` instead
//! cites: 5, 4.8
//! expect: run
//! output: 2070
//! notes: Pins #9234 (pick 93797636b): FirstOrNull, LastOrNull and SingleOrNull are the non-throwing forms
//!   of First, Last and Single over a Dictionary<K, V>, which is an ICollection<DictionaryEntry<K,
//!   V>> and so carries the 5 element-access members; the unfixed compiler returned the wrong value
//!   or lowered nothing. Per iteration the one-entry dictionary's key k<i> has length 1 + digits(i)
//!   and its value v<i> the same: total = sum over i of 3 * (1 + digits(i)); i = 0..9 add 10 * 6 =
//!   60, i = 10..99 add 90 * 9 = 810, i = 100..199 add 100 * 12 = 1200; 2070. The empty dictionary
//!   never adds 1000.
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0 ..< 200 do
      using var d := new Dictionary<string, string>();
      d[$"k{i}"] := $"v{i}";
      var a := d.FirstOrNull();
      var b := d.LastOrNull();
      var c := d.SingleOrNull();
      if a <> null and b <> null and c <> null then
        total := total + a.Key.Length + b.Value.Length + c.Key.Length;
      end if;
      using var e := new Dictionary<string, string>();
      var z := e.FirstOrNull();
      if z <> null then
        total := total + 1000;
      end if;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
