//! case: s26_5_9705
//! doc: spec
//! section: 26.5
//! title: a member TextWriter does not declare is rejected on Console.Out through . as a call
//! rule: Where neither rule 1 nor this search supplies the name, nothing supplies it: the access names no
//!   member and is a compile error at the name, the diagnostic naming the receiver's static type
//! cites: 26.5, stdlib 7.4
//! expect: reject
//! diagnostic: no member
//! notes: Pins #9223's agreement requirement: the `.` and `?.` spellings of one undeclared member are
//!   refused with the same rule and the same sentence, so a fix that replaced the `.` refusal instead
//!   of adding the `?.` one would fail here. The `?.` spelling is s26_5_9701.
module Conf;

public class Program
  public static method Main()
    Console.Out.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
