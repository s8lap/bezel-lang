//! case: l4_17_2001
//! doc: stdlib
//! section: 4.17
//! title: SortedDictionary with string? values replaces on indexer set and
//!   drops a removed entry
//! rule: set: insert or replace
//! cites: 4.8, 28.9
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
      var d := new SortedDictionary<int, string?>();
      d[1] := "a" + (40 + i mod 10).ToString();
      d[1] := "b" + (40 + i mod 10).ToString();
      d.Add(2, "c" + i.ToString());
      d.Remove(2);
      tot := tot + (d[1] ?? "").Length;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
