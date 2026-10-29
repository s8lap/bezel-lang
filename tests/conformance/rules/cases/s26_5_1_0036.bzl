//! case: s26_5_1_0036
//! doc: spec
//! section: 26.5.1
//! title: control: a user extension on Array<string> does not affect an Array<int> receiver
//! rule: An extension applies only where the receiver's compile-time type matches its head (spec 26.5 rule 2, v0.350): a user extension on Array<string> is not a candidate for an Array<int> receiver, so the standard library Sum is reached.
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
