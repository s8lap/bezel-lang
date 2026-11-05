//! case: s27_6_1407
//! doc: spec
//! section: 27.6
//! title: a bound reference over a string narrowed from a nullable parameter is formed on the non-null value
//! rule: the reference is a bound reference: the receiver is captured at formation as the
//!   instance-method sentence above states (by reference for a class- or interface-typed
//!   receiver, by copy for a value-shaped one — a record, an enum, a copyable choice, a
//!   non-owning inline array, a primitive, the unit value, a `string` or a function value
//! cites: 27.6, 26.5
//! expect: run
//! stdout: z!,none
module Conf;

extension string
  public method Shout(): string => this + "!";
end extension;

public class Program
  static method Id(s: string?): string
    if s <> null then
      var f: () -> string := s.Shout;
      return f();
    end;
    return "none";
  end Id;

  public static method Main()
    Console.WriteLine(Id("z") + "," + Id(null));
  end Main;
end Program;
