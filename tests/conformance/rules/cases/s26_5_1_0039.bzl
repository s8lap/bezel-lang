//! case: s26_5_1_0039
//! doc: spec
//! section: 26.5.1
//! title: user extensions named Take, Skip, Where, Select on Stack<int> outrank the library
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   79
//!   80
//!   81
//! issue: 7663
module R;
extension Stack<int> public method Take(n: int): int => 77; public method Skip(n: int): int => 79; public method Where(f: (int) -> bool): int => 80; public method Select(f: (int) -> int): int => 81; end;
class Program public static method Main(): int
var s := new Stack<int>(); s.Push(1); s.Push(2);
Console.WriteLine(s.Take(1)); Console.WriteLine(s.Skip(1));
Console.WriteLine(s.Where(lambda(x) => x > 1)); Console.WriteLine(s.Select(lambda(x) => x));
return 0;
end Main; end Program;
