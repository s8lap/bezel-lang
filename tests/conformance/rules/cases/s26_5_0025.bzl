//! case: s26_5_0025
//! doc: spec
//! section: 26.5
//! title: Array declares an instance Count, so a same-named user extension property is never consulted
//! rule: Instance members win by name: if the receiver's declared type declares any instance member named Foo, instance resolution is committed and extension blocks are never consulted for that name on that receiver (spec 26.5 rule 1, v0.350). A same-named user extension is never selected.
//! cites: 26.5
//! expect: run
//! stdout: 3
module R;
extension Array<int> public property Count: int => 77; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3];
  Console.WriteLine(a.Count);
  return 0;
end Main; end Program;
