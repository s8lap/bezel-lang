//! case: s27_6_0019
//! doc: spec
//! section: 27.6
//! title: the same reference at a type argument that conforms to IComparable is formed and invoked
//! rule: The formation is an instantiation site: its constraints are read at the formation exactly as at a call with that list; string conforms to IComparable<string>.
//! cites: 27.6, 14.2
//! expect: run
//! stdout: a
//! issue: 5814
module Conf;

public class Program
  public static method Main()
    var m: (string, string) -> string := Math.Min<string>;
    Console.WriteLine(m("b", "a"));
  end Main;
end Program;
