//! case: s7_7_2001
//! doc: spec
//! section: 7.7
//! title: ??= on a Dictionary indexer of a string? value assigns only the
//!   first time the stored value is null
//! rule: The compound form `??=` evaluates its right-hand side, and invokes
//!   the setter, only when the assignment occurs
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
      var d := new Dictionary<int, string?>();
      d[1] := null;
      d[1] ??= "c" + (40 + i mod 10).ToString();
      d[1] ??= "never" + i.ToString();
      tot := tot + (d[1] ?? "").Length;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
