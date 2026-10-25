//! case: l3_1_0054
//! doc: stdlib
//! section: 3.1
//! title: a named argument to a StringBuilder member binds to the parameter it names
//! rule: stdlib §3.1 declares `Replace(oldValue: string, newValue: string):
//!   StringBuilder`. §22.5: named arguments may appear in any order after
//!   positional arguments, and each supplied argument binds exactly one parameter
//!   - so `Replace(oldValue := "l", newValue := "L")` binds "l" to oldValue and
//!   "L" to newValue, and the builder holds the text with its first "l" replaced.
//! cites: 3.1, 22.5
//! expect: run
//! stdout: heLLo
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("hello");
    sb.Replace(oldValue := "l", newValue := "L");
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
