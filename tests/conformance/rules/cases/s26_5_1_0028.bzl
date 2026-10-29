//! case: s26_5_1_0028
//! doc: spec
//! section: 26.5.1
//! title: a user extension on LinkedList<int> outranks the standard library Sum
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 77
module R;
extension LinkedList<int> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var l := new LinkedList<int>(); l.AddLast(5); l.AddLast(6);
  Console.WriteLine(l.Sum());
  return 0;
end Main; end Program;
