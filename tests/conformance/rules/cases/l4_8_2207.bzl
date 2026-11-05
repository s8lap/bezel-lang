//! case: l4_8_2207
//! doc: stdlib
//! section: 4.8
//! title: TryGetValue with a string key: the hit overwrites the holding
//!   local, the miss assigns null
//! rule: the false path assigns `null`
//! cites: 1
//! expect: run
//! stdout: 300
//! issue: 8268
//! notes: Trace per iteration: the hit stores the 3-character value (+3);
//!   the miss on the absent key "nokey" returns false and assigns null
//!   (stdlib 1 shape (a)), so the closing read adds (null ??
//!   "").Length = 0. 100 iterations, so 300. Aborts with a refcount
//!   over-release on the pre-fix compiler (parent of a29fb10c), passes
//!   on the staging tip.
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 100 do
      var d := new Dictionary<string, string>();
      d["k" + i.ToString()] := "a" + (40 + i mod 10).ToString();
      var v: string? := "init" + i.ToString();
      if d.TryGetValue("k" + i.ToString(), out v) then
        tot := tot + (v ?? "").Length;
      end;
      if d.TryGetValue("nokey", out v) then
        tot := tot + 1000;
      end;
      tot := tot + (v ?? "").Length;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
