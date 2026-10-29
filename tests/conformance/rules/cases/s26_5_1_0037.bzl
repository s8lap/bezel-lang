//! case: s26_5_1_0037
//! doc: spec
//! section: 26.5.1
//! title: user exts with arguments on Array<int> outrank the library ElementAt, Aggregate, Zip and FirstOrNull
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 78
//!   81
//!   82
//!   83
module R;
extension Array<int> public method ElementAt(i: int): int => 78; public method Aggregate(s: int, f: (int, int) -> int): int => 81; public method Zip(o: Array<int>): int => 82; public method FirstOrNull(): int => 83; end;
class Program public static method Main(): int
var a: Array<int> := [1,2,3];
Console.WriteLine(a.ElementAt(1)); Console.WriteLine(a.Aggregate(0, lambda(s, x) => s + x));
Console.WriteLine(a.Zip(a)); Console.WriteLine(a.FirstOrNull());
return 0;
end Main; end Program;
