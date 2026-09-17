//! case: s26_5_1_0002
//! doc: spec
//! section: 26.5.1
//! title: an extension on a derived interface outranks one on the interface
//!   it extends
//! rule: An extension on E1 is more specific than one on E2 when E1 is a
//!   subtype of E2 and E2 is not a subtype of E1. Derived interface beats
//!   extended interface: `IReadOnlyCollection<T>` extends `IEnumerable<T>`
//!   (stdlib §4.1), and `Array<int>` implements both.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: derived
module Conf;

extension IEnumerable<T>
  public method Kind(): string => "base";
end;

extension IReadOnlyCollection<T>
  public method Kind(): string => "derived";
end;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(xs.Kind());
  end Main;
end Program;
