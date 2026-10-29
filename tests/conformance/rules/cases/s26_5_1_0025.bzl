//! case: s26_5_1_0025
//! doc: spec
//! section: 26.5.1
//! title: a user extension on IList<int> applies at the IList and Array static types but not at IReadOnlyList
//! rule: Extensions are resolved on the compile-time type of the receiver (spec 26.5, v0.350). A user extension on IList<int> is more specific than the library's on IEnumerable<T> for an IList<int> receiver and for an Array<int> receiver (Array<int> implements IList<int>); an IReadOnlyList<int> receiver is not an IList<int>, so it reaches the library Sum.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 77
//!   6
//!   77
module R;
extension IList<int> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  var l: IList<int> := a;
  var r: IReadOnlyList<int> := a;
  Console.WriteLine(l.Sum()); Console.WriteLine(r.Sum()); Console.WriteLine(a.Sum());
  return 0;
end Main; end Program;
