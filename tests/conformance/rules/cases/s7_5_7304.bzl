//! case: s7_5_7304
//! doc: spec
//! section: 7.5
//! title: `?.Value` over a LinkedList<int?> yields null for a null element,
//!   never a wrapped null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access. The result type is always nullable
//! cites: 7.7
//! expect: run
//! output: -1
//!   4
//! notes: Pins the #8770 fix at an element type that is itself nullable
//!   (LinkedList<int?>): the result of `m?.FirstNode?.Value` is `int?`, never
//!   a doubly wrapped value, so the first element (null) reads as absent and
//!   the last (4) as present.
module Conf;

public class Program
  public static method Main()
    using var l := new LinkedList<int?>();
    var a := l.AddLast(null);
    var b := l.AddLast(4);
    var m: LinkedList<int?>? := l;
    var x: int? := m?.FirstNode?.Value;
    var y: int? := m?.LastNode?.Value;
    Console.WriteLine(x ?? -1);
    Console.WriteLine(y ?? -1);
  end Main;
end Program;
