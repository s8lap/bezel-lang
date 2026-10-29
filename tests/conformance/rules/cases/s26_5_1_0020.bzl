//! case: s26_5_1_0020
//! doc: spec
//! section: 26.5.1
//! title: a user Sum(selector) on Array<int> outranks the library one and leaves Sum() to the library
//! rule: Extension resolution ranks only applicable candidates by arity and argument types (spec 26.5.1, v0.350): the user Sum(selector) on Array<int> is more specific than the standard library Sum(selector) on IEnumerable<T>, and does not apply to the zero-argument call, which reaches the standard library.
//! cites: 26.5.1, 26.5
//! expect: run
//! output: 1000
//!   6
module R;
extension Array<int> public method Sum(selector: (int) -> int): int => 1000; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Sum(lambda(x) => x * 2)); Console.WriteLine(a.Sum());
  return 0;
end Main; end Program;
