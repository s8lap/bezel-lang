//! case: s26_5_1812
//! doc: spec
//! section: 26.5
//! title: an extension ToArray over FrozenArray<int> is never consulted beside the declared instance ToArray
//! rule: instance resolution is committed and extension blocks are **never
//!   consulted** for that name on that receiver
//! cites: 4.19
//! expect: run
//! output: 3
//!   3
//! issue: 8104
module Conf;

extension FrozenArray<int>
  public method ToArray(): int => 98;
end;

class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var f: FrozenArray<int> := FrozenArray<int>.From(a);
    using var b := f.ToArray();
    Console.WriteLine(b.Count);
    Console.WriteLine(b[2]);
  end Main;
end Program;
