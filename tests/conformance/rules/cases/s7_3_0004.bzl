//! case: s7_3_0004
//! doc: spec
//! section: 7.3
//! title: a named argument carrying a non-null value at a non-nullable parameter is admitted
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 7.3, 7.1, 22.5
//! expect: run
//! output: 3
//! issue: 7995
module Conf;
import System.IO;

public class Program
  public static method Main(): int
    var p := Path.Combine(a := "A", b := "B");
    Console.WriteLine(p.Length);
    return 0;
  end Main;
end Program;
