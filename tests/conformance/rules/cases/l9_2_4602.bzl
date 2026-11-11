//! case: l9_2_4602
//! doc: stdlib
//! section: 9.2
//! title: double.ParseOrNull is null for near-miss non-finite spellings
//! rule: They are matched exactly, so `"nan"`, `"inf"`, `"infinity"`, `"INFINITY"`, `"+Infinity"` and `"-NaN"` raise `FormatException`.
//! cites: 9.2
//! expect: run
//! stdout: null null null null null null
module Conf;

public class Program
  public static method Main()
    var a := double.ParseOrNull("nan");
    var b := double.ParseOrNull("inf");
    var c := double.ParseOrNull("infinity");
    var d := double.ParseOrNull("+Infinity");
    var e := double.ParseOrNull("-NaN");
    var f := double.ParseOrNull("INFINITY");
    Console.WriteLine($"{a} {b} {c} {d} {e} {f}");
  end Main;
end Program;
