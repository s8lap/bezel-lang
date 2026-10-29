//! case: s26_5_0026
//! doc: spec
//! section: 26.5
//! title: Dictionary declares an instance ContainsKey, so a same-named user extension is never consulted
//! rule: Instance members win by name: if the receiver's declared type declares any instance member named Foo, instance resolution is committed and extension blocks are never consulted for that name on that receiver (spec 26.5 rule 1, v0.350). A same-named user extension is never selected.
//! cites: 26.5
//! expect: run
//! stdout: True
module R;
extension Dictionary<string, int> public method ContainsKey(k: string): bool => false; end;
class Program public static method Main(): int
  var d := new Dictionary<string, int>(); d.Add("a", 1);
  Console.WriteLine(d.ContainsKey("a"));
  return 0;
end Main; end Program;
