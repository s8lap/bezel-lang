//! case: s26_5_9701
//! doc: spec
//! section: 26.5
//! title: a member TextWriter does not declare is rejected on Console.Out through ?. as a call
//! rule: with a `?.` or postfix-`!` receiver read at its underlying non-nullable type
//! cites: 26.5, 7.5, stdlib 7.4
//! expect: reject
//! diagnostic: no member
//! notes: Pins #9223 (pick ae15768e4): the `?.` spelling reads its receiver at the underlying non-nullable
//!   type, so `Bogus` is looked up on `TextWriter` exactly as the `.` spelling does (s26_5_9705,
//!   s26_5_9402) and, supplied by neither rule, is a compile error at the name. Accepting twin:
//!   s26_5_9706 (declared member `Flush`).
module Conf;

public class Program
  public static method Main()
    Console.Out?.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
