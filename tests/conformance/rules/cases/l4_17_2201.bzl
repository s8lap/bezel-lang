//! case: l4_17_2201
//! doc: stdlib
//! section: 4.17
//! title: SortedDictionary.TryGetValue into a pre-declared holding string?
//!   local reads the stored value
//! rule: public method TryGetValue(key: K, out value: V?): bool;
//! cites: 1
//! expect: run
//! stdout: 600
//! notes: Trace: 200 iterations, each hit adds the 3-character stored
//!   string ("a40".."a49"), so 600. Aborts with a refcount
//!   over-release on the pre-fix compiler (parent of a29fb10c), passes
//!   on the staging tip.
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 200 do
      var c := new SortedDictionary<int, string>();
      c[5] := "z"; c[1] := "a" + (40 + i mod 10).ToString();
      var v: string? := "init" + i.ToString();
      if c.TryGetValue(1, out v) then tot := tot + (v ?? "").Length; end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
