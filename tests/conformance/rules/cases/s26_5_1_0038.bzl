//! case: s26_5_1_0038
//! doc: spec
//! section: 26.5.1
//! title: user extensions named Take, Skip, Where, Select on Array<int> outrank the library
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   79
//!   80
//!   81
//! issue: 7663
module R;
extension Array<int> public method Take(n: int): int => 77; public method Skip(n: int): int => 79; public method Where(f: (int) -> bool): int => 80; public method Select(f: (int) -> int): int => 81; end;
class Program public static method Main(): int
var a: Array<int> := [1,2,3];
Console.WriteLine(a.Take(1)); Console.WriteLine(a.Skip(1));
Console.WriteLine(a.Where(lambda(x) => x > 1)); Console.WriteLine(a.Select(lambda(x) => x));
return 0;
end Main; end Program;
