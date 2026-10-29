//! case: s26_5_0023
//! doc: spec
//! section: 26.5
//! title: Set declares an instance TryAdd (stdlib 4.10), so a same-named user extension is never consulted
//! rule: Instance members win by name: if the receiver's declared type (or any base type) declares any instance member named Foo, instance resolution is committed and extension blocks are never consulted for that name on that receiver (spec 26.5 rule 1, v0.350). Set<T> itself declares TryAdd(item: T): bool (stdlib 4.10), so the user extension TryAdd is never selected and a fresh element is added (True).
//! cites: 26.5
//! expect: run
//! stdout: True
module R;
extension Set<int> public method TryAdd(x: int): bool => false; end;
class Program public static method Main(): int
  var s := new Set<int>(); s.Add(1);
  Console.WriteLine(s.TryAdd(2));
  return 0;
end Main; end Program;
