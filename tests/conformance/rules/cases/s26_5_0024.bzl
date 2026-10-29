//! case: s26_5_0024
//! doc: spec
//! section: 26.5
//! title: LinkedList declares an instance Last, so a same-named user extension is never consulted
//! rule: Instance members win by name: if the receiver's declared type declares any instance member named Foo, instance resolution is committed and extension blocks are never consulted for that name on that receiver (spec 26.5 rule 1, v0.350). A same-named user extension is never selected.
//! cites: 26.5
//! expect: run
//! stdout: 6
module R;
extension LinkedList<int> public method Last(): int => 77; end;
class Program public static method Main(): int
  var l := new LinkedList<int>(); l.AddLast(5); l.AddLast(6);
  Console.WriteLine(l.Last ?? -1);
  return 0;
end Main; end Program;
