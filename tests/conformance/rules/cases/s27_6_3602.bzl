//! case: s27_6_3602
//! doc: spec
//! section: 27.6
//! title: a type-qualified reference to an instance property is the unbound formation
//! rule: A type-qualified `T.M` reaches … a nested type, an enum constant, a static member — or is the
//!   type-qualified formation above over an instance property or an extension method (`string.Length`,
//!   `IEnumerable<int>.Count`), the unbound references of this section
//! cites: 27.6, 26.5
//! expect: run
//! stdout: 3
//! issue: 8065
module Conf;

public class Program
  public static method Main()
    var f := string.Length;
    Console.WriteLine(f("abc"));
  end Main;
end Program;
