//! case: s7_3_0003
//! doc: spec
//! section: 7.3
//! title: a named argument carrying null at a non-nullable parameter is rejected
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 7.3, 7.1, 22.5
//! expect: reject
//! diagnostic: non-nullable
//! issue: 7995
module Conf;
import System.IO;

public class Program
  public static method Main(): int
    var p := Path.Combine(a := null, b := "x");
    Console.WriteLine(p);
    return 0;
  end Main;
end Program;
