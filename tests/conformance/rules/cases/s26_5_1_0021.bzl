//! case: s26_5_1_0021
//! doc: spec
//! section: 26.5.1
//! title: a named-argument call selects the user Sum(selector) extension on Array<int>
//! rule: Extension resolution ranks only applicable candidates by arity and argument types (spec 26.5.1, v0.350): the user Sum(selector) on Array<int> is more specific than the standard library Sum(selector) on IEnumerable<T>, and does not apply to the zero-argument call, which reaches the standard library.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 1000
module R;
extension Array<int> public method Sum(selector: (int) -> int): int => 1000; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Sum(selector := lambda(x) => x * 2));
  return 0;
end Main; end Program;
