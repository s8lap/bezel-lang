//! case: l4_8_2003
//! doc: stdlib
//! section: 4.8
//! title: TryGetValue on a Dictionary with string? values yields the stored
//!   value in an out var
//! rule: Use `GetOrNull` (the §1 `*OrNull` tier) or `TryGetValue` for safe
//!   lookup.
//! cites: 1, 28.9
//! expect: run
//! stdout: 600
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
      if d.TryGetValue(1, out var v) then
        tot := tot + (v ?? "").Length;
      end;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
