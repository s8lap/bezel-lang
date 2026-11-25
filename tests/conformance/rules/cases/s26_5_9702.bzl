//! case: s26_5_9702
//! doc: spec
//! section: 26.5
//! title: a member TextReader does not declare is rejected on Console.In through ?. as a call
//! rule: with a `?.` or postfix-`!` receiver read at its underlying non-nullable type
//! cites: 26.5, 7.5, stdlib 7.4
//! expect: reject
//! diagnostic: no member
//! notes: Pins #9223: `Peek` is a Stack/Queue member, not a `TextReader` member (stdlib 7.4 declares Read,
//!   ReadLine and ReadToEnd), and `TextReader` declares no base type, so nothing supplies the name
//!   through `?.` either. The `.` spelling is s26_5_9401.
module Conf;

public class Program
  public static method Main()
    Console.In?.Peek();
    Console.WriteLine("after");
  end Main;
end Program;
