//! case: s26_5_1_0010
//! doc: spec
//! section: 26.5.1
//! title: a user extension on Set<int> outranks the standard library Sum
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1; the test runs on the receiver-substituted heads, and a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection is therefore selected over the standard library's extension on IEnumerable<T> for the same name.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 77
module R;
extension Set<int> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var s := new Set<int>(); s.Add(1); s.Add(2);
  Console.WriteLine(s.Sum());
  return 0;
end Main; end Program;
