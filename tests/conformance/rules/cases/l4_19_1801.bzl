//! case: l4_19_1801
//! doc: stdlib
//! section: 4.19
//! title: ToArray on an annotated FrozenArray<int> returns a fresh mutable copy
//! rule: public method ToArray(): owned Array<T>;
//! cites: 26.5
//! expect: run
//! output: 3
//!   3
//! issue: 8104
module Conf;

class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var f: FrozenArray<int> := FrozenArray<int>.From(a);
    using var b := f.ToArray();
    Console.WriteLine(b.Count);
    Console.WriteLine(b[2]);
  end Main;
end Program;
