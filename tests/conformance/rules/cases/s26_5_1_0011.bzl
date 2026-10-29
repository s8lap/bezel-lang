//! case: s26_5_1_0011
//! doc: spec
//! section: 26.5.1
//! title: a user generic extension on Array<T> outranks the standard library extension on IEnumerable<T>
//! rule: An extension on E1 is more specific than one on E2 when E1 is a subtype of E2 and E2 is not a subtype of E1; the test runs on the receiver-substituted heads, and a class beats an interface it implements (spec 26.5.1, v0.350). A user extension on a built-in collection is therefore selected over the standard library's extension on IEnumerable<T> for the same name.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 3
module R;
extension Array<T> public method Sum(): int => this.Count; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Sum());
  return 0;
end Main; end Program;
