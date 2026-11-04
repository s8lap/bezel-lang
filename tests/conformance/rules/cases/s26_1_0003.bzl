//! case: s26_1_0003
//! doc: spec
//! section: 26.1
//! title: `this` in an extension over DateTime reaches a built-in DateTime parameter as the receiver
//! rule: `this` refers to the receiver.
//! cites: 26.1
//! expect: run
//! issue: 7304
//! output: True
//!   False

module Conf;

extension DateTime
  public method Cmp(other: DateTime): int => other.CompareTo(this);
end;

public class Program
  public static method Main()
    var a := new DateTime(2020, 1, 1);
    var b := new DateTime(2021, 1, 1);
    Console.WriteLine(a.Cmp(b) > 0);
    Console.WriteLine(b.Cmp(a) > 0);
  end Main;
end Program;
