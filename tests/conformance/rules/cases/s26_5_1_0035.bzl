//! case: s26_5_1_0035
//! doc: spec
//! section: 26.5.1
//! title: null-conditional calls: user Sum is selected, a null receiver yields null, instance Contains wins
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>. Instance members win by name (spec 26.5 rule 1).
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   -1
//!   True
//! issue: 7700
module R;
extension Array<int> public method Sum(): int => 77; public method Contains(x: int): bool => false; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  var n: Array<int>? := a;
  var z: Array<int>? := null;
  Console.WriteLine(n?.Sum() ?? -1);
  Console.WriteLine(z?.Sum() ?? -1);
  Console.WriteLine(n?.Contains(2) ?? false);
  return 0;
end Main; end Program;
