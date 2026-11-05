//! case: s27_6_1401
//! doc: spec
//! section: 27.6
//! title: a receiver-qualified reference to an extension method over a string local is bound and callable
//! rule: the reference is a bound reference: the receiver is captured at formation as the
//!   instance-method sentence above states (by reference for a class- or interface-typed
//!   receiver, by copy for a value-shaped one — a record, an enum, a copyable choice, a
//!   non-owning inline array, a primitive, the unit value, a `string` or a function value
//! cites: 27.6, 26.5
//! expect: run
//! stdout: hi!
module Conf;

extension string
  public method Shout(): string => this + "!";
end extension;

public class Program
  public static method Main()
    var s := "hi";
    var f: () -> string := s.Shout;
    Console.WriteLine(f());
  end Main;
end Program;
