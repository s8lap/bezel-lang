//! case: s28_11_9912
//! doc: spec
//! section: 28.11
//! title: A DictionaryEntry local outside a loop keeps its Key after repeated captures
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` —
//!   keeps
//! cites: 28.11, 28.9, 27.5
//! expect: run
//! stdout: 2200 xy
//! notes: Harvested from the #9252 review (a12_entry_outside_loop_always). The entry is declared outside
//!   the loop and a closure over it is formed on each of 200 iterations: 11 each, 2200, and the Key
//!   is still readable after the loop. The Key leak itself is not observable from stdout; this guards
//!   an early or repeated release of the entry's Key.
module Conf;

public class Program
  public static method Main(): int
    var t := 0;
    using var d := new Dictionary<string, int>();
    d.Add("xy", 9);
    var fe := d.First();
    var i := 0;
    while i < 200 do
      if i >= 0 then
        var g := lambda() => fe.Key.Length + fe.Value;
        t := t + g();
      end if;
      i := i + 1;
    end while;
    Console.WriteLine($"{t} {fe.Key}");
    return 0;
  end Main;
end Program;
