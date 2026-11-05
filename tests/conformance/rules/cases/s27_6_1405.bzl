//! case: s27_6_1405
//! doc: spec
//! section: 27.6
//! title: a bound reference may be formed over a temporary string receiver and called repeatedly
//! rule: the reference is a bound reference: the receiver is captured at formation as the
//!   instance-method sentence above states (by reference for a class- or interface-typed
//!   receiver, by copy for a value-shaped one — a record, an enum, a copyable choice, a
//!   non-owning inline array, a primitive, the unit value, a `string` or a function value
//! cites: 27.6
//! expect: run
//! stdout: 220
module Conf;

extension string
  public method Shout(): string => this + "!";
end extension;

public class Program
  static method Mk(i: int): string => "abc" + i.ToString();

  public static method Main()
    var n := 0;
    for var i in 0..<20 do
      var f: () -> string := Mk(i).Shout;
      n += f().Length;
      n += f().Length;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
