//! case: l4_8_2001
//! doc: stdlib
//! section: 4.8
//! title: a closure that writes a string? value through the indexer updates
//!   the same key and adds a new one
//! rule: `dict[key] := value` inserts the key or updates an existing one
//!   (add-or-update)
//! cites: 27.2, 28.9
//! expect: run
//! stdout: 1000
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
      var put := lambda(k: int) => do
        d[k] := "c" + (40 + k mod 10).ToString();
      end;
      put(1);
      put(1);
      put(2);
      tot := tot + (d[1] ?? "").Length + d.Count;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
