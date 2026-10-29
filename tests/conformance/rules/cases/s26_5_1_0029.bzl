//! case: s26_5_1_0029
//! doc: spec
//! section: 26.5.1
//! title: a user ext on SortedSet<int> outranks the library Sum, and the Last member stays an instance member
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1, evaluated on the receiver-substituted heads; a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection class therefore outranks the standard library's extension on IEnumerable<T>.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   6
module R;
extension SortedSet<int> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var s := new SortedSet<int>(); s.Add(5); s.Add(6);
  Console.WriteLine(s.Sum());
  Console.WriteLine(s.Last);
  return 0;
end Main; end Program;
