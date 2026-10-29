//! case: s26_5_1_0024
//! doc: spec
//! section: 26.5.1
//! title: a user extension on Array<int> applies at the Array type only, not at IList, IReadOnlyList or Where
//! rule: Extensions are resolved on the compile-time type of the receiver (spec 26.5, v0.350); the most specific substituted head wins (spec 26.5.1). With a user extension on Array<int>: an IList<int> or IReadOnlyList<int> receiver reaches the standard library Sum, an Array<int> receiver reaches the user Sum, and the result of Where is an IEnumerable<int>, again the library.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 6
//!   6
//!   77
//!   5
module R;
extension Array<int> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  var l: IList<int> := a;
  var r: IReadOnlyList<int> := a;
  Console.WriteLine(l.Sum()); Console.WriteLine(r.Sum()); Console.WriteLine(a.Sum());
  Console.WriteLine(a.Where(lambda(x) => x > 1).Sum());
  return 0;
end Main; end Program;
