//! case: l4_8_2209
//! doc: stdlib
//! section: 4.8
//! title: a dictionary of string? values reports Count 1 after one indexer
//!   store per fresh map
//! rule: pure readonly property Count: int;
//! cites: 4.1
//! expect: run
//! stdout: 200
//! notes: Trace: each of 200 fresh maps receives one indexer store (set
//!   inserts), so Count is 1 and the sum is 200. Green on the pre-fix
//!   compiler and the tip: a control for the string? value path.
module Conf;

public class Program
  public static method Main()
    var tot := 0;
    var i := 0;
    while i < 200 do
      var c := new Dictionary<int, string?>();
      c[1] := "a" + (40 + i mod 10).ToString();
      tot := tot + c.Count;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
