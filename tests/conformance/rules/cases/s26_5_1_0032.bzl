//! case: s26_5_1_0032
//! doc: spec
//! section: 26.5.1
//! title: user extensions on Stack<int> and Queue<int> outrank the standard library
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   78
module R;
extension Stack<int> public method Sum(): int => 77; end;
extension Queue<int> public method Max(): int => 78; end;
class Program public static method Main(): int
  var s := new Stack<int>(); s.Push(1);
  var q := new Queue<int>(); q.Enqueue(1);
  Console.WriteLine(s.Sum()); Console.WriteLine(q.Max());
  return 0;
end Main; end Program;
