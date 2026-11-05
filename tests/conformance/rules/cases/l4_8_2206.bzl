//! case: l4_8_2206
//! doc: stdlib
//! section: 4.8
//! title: TryGetValue on a dictionary declared through a type alias, into a
//!   pre-declared local
//! rule: public method TryGetValue(key: K, out value: V?): bool;
//! cites: 1
//! expect: run
//! stdout: 300
//! issue: 8268
//! notes: Trace: 100 iterations, each hit adds the 3-character stored
//!   string, so 300. Aborts with a refcount over-release on the
//!   pre-fix compiler (parent of a29fb10c), passes on the staging tip.
module Conf;

type Names := Dictionary<int, string>;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 100 do
      var d: Names := new Dictionary<int, string>();
      d[1] := "a" + (40 + i mod 10).ToString();
      var v: string? := null;
      if d.TryGetValue(1, out v) then
        tot := tot + (v ?? "").Length;
      end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
