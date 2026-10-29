//! case: s26_5_1_0041
//! doc: spec
//! section: 26.5.1
//! title: user extensions Max and Sum on FrozenArray<int> outrank the standard library
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   78
//! issue: 7710
module R;
extension FrozenArray<int> public method Max(): int => 77; public method Sum(): int => 78; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  using var f := FrozenArray<int>.From(a);
  Console.WriteLine(f.Max()); Console.WriteLine(f.Sum());
  return 0;
end Main; end Program;
