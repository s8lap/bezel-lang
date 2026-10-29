//! case: s26_5_1_0033
//! doc: spec
//! section: 26.5.1
//! title: user extensions on OwningArray<T> outrank the standard library for an OwningArray receiver
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   78
module R;
class Foo
  public constructor() end;
end Foo;
extension OwningArray<T> where T: class public method Any(): int => 77; public method First(): int => 78; end;
class Program public static method Main(): int
  using var a := new OwningArray<Foo>(); a.Add(new Foo());
  Console.WriteLine(a.Any()); Console.WriteLine(a.First());
  return 0;
end Main; end Program;
