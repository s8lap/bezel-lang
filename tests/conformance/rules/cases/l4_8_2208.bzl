//! case: l4_8_2208
//! doc: stdlib
//! section: 4.8
//! title: TryGetValue as a while condition: the first miss assigns null to
//!   the recipient
//! rule: the false path assigns `null`
//! cites: 1
//! expect: run
//! stdout: 21
//! issue: 8268
//! notes: Trace: keys 0..4 map to "v100".."v104" (4 characters each). i =
//!   0..4 hit (+4 each, 20); at i = 5 the key 5 is absent, TryGetValue
//!   returns false and assigns null and the loop ends; the closing
//!   read (v ?? "x").Length is 1. 20 + 1 = 21. Aborts with a refcount
//!   over-release on the pre-fix compiler (parent of a29fb10c), passes
//!   on the staging tip.
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var d := new Dictionary<int, string>();
    var k := 0;
    while k < 5 do d[k] := "v" + (100 + k).ToString(); k := k + 1; end;
    var i := 0;
    var v: string? := null;
    while i < 200 and d.TryGetValue(i mod 7, out v) do
      tot := tot + (v ?? "").Length;
      i := i + 1;
    end;
    Console.WriteLine(tot + (v ?? "x").Length);
  end Main;
end Program;
