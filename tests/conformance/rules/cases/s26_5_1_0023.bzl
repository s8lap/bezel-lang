//! case: s26_5_1_0023
//! doc: spec
//! section: 26.5.1
//! title: a user extension on Array<string> is not a candidate for an Array<int> receiver
//! rule: An extension applies only where the receiver's compile-time type matches its head, and extensions are resolved on the compile-time type of the receiver (spec 26.5 and 26.5.1, v0.350). A user extension on Array<int> is not a candidate for an IEnumerable<int> receiver or for an Array<string> head mismatch, so the standard library Sum applies.
//! cites: 26.5.1, 26.5
//! expect: run
//! stdout: 6
module R;
extension Array<string> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Sum());
  return 0;
end Main; end Program;
