//! case: s26_5_1_0034
//! doc: spec
//! section: 26.5.1
//! title: a user generic extension on LinkedList<T> is selected at two instantiations
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   77
module R;
extension LinkedList<T> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var l := new LinkedList<int>(); l.AddLast(5);
  var m := new LinkedList<string>(); m.AddLast("a");
  Console.WriteLine(l.Sum()); Console.WriteLine(m.Sum());
  return 0;
end Main; end Program;
