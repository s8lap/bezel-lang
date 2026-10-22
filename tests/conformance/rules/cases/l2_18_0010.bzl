//! case: l2_18_0010
//! doc: stdlib
//! section: 2.18
//! title: an unrecognized specifier in an interpolation hole raises FormatException
//! rule: "An unrecognized specifier raises `FormatException`" ([stdlib §2.18]). `c` is the TimeSpan constant form ("`c` renders the span's magnitude behind at most one sign"), so the specifier `g` in the hole is unrecognized and the interpolation throws before the next statement runs.
//! expect: run
//! output: FE-interp
module Conf;
public class Program
  public static method Main(): int
    var a := TimeSpan.FromSeconds(5.0);
    try Console.WriteLine($"{a:g}"); Console.WriteLine("noraise"); catch FormatException => Console.WriteLine("FE-interp"); end try;
    return 0;
  end Main;
end Program;
