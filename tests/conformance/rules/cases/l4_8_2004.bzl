//! case: l4_8_2004
//! doc: stdlib
//! section: 4.8
//! title: GetOrNull and the indexer on a Dictionary with string? values
//!   both read the stored value
//! rule: `null` when the key is absent, otherwise the value under the
//!   family's read semantics
//! cites: 4.8, 28.9
//! expect: run
//! stdout: 1200
//! notes: Exercises a #8207 path (a Dictionary value of type string?); the
//!   release of the +1 string is not stdout-observable, only the
//!   stored values are
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 200 do
      var d := new Dictionary<int, string?>();
      d[1] := "a" + (40 + i mod 10).ToString();
      var v := d.GetOrNull(1);
      tot := tot + (v ?? "").Length;
      var w: string? := d[1];
      tot := tot + (w ?? "").Length;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
