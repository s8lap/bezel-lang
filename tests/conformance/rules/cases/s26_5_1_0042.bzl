//! case: s26_5_1_0042
//! doc: spec
//! section: 26.5.1
//! title: user extensions Foo, Any and First on FrozenArray<int> are all dispatched
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   78
//!   79
//! issue: 7710
module R;
extension FrozenArray<int> public method Foo(): int => 77; public method Any(): int => 78; public method First(): int => 79; public method Count2(): int => 5; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  using var f := FrozenArray<int>.From(a);
  Console.WriteLine(f.Foo()); Console.WriteLine(f.Any()); Console.WriteLine(f.First());
  return 0;
end Main; end Program;
