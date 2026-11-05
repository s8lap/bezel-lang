//! case: l4_8_2204
//! doc: stdlib
//! section: 4.8
//! title: TryGetValue over a string? value type into a pre-declared null
//!   local reads the stored value
//! rule: public method TryGetValue(key: K, out value: V?): bool;
//! cites: 1
//! expect: run
//! stdout: 600
//! issue: 8268
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
      var c := new Dictionary<int, string?>();
      c[1] := "a" + (40 + i mod 10).ToString();
      var v: string? := null;
      if c.TryGetValue(1, out v) then tot := tot + (v ?? "").Length; end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
