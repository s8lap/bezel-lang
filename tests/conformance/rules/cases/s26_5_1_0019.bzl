//! case: s26_5_1_0019
//! doc: spec
//! section: 26.5.1
//! title: a user Sum(x) extension on Array<int> does not shadow the library Sum(), call order reversed
//! rule: Extension resolution ranks only the applicable candidates (spec 26.5.1, v0.350): a user extension on Array<int> that does not match the call's arity is not a candidate, so the call falls to the standard library extension on IEnumerable<T>; a call that matches it selects the more specific user extension.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 5
//!   6
module R;
extension Array<int> public method Sum(x: int): int => x; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Sum(5)); Console.WriteLine(a.Sum());
  return 0;
end Main; end Program;
