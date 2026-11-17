//! case: s7_5_6242
//! doc: spec
//! section: 7.5
//! title: ?.Sort with an un-annotated comparer lambda through a ?. chain
//!   sorts in place
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access.
//! cites: 7.5, 4.4
//! expect: run
//! stdout: a,b,c
module Conf;

record W(K: string, V: int);
record H(Items: Array<W>?);

public class Program
  public static method Main()
    var a: Array<W> := [new W("b", 1), new W("c", 2), new W("a", 3)];
    var h: H? := new H(a);
    h?.Items?.Sort(lambda(x, y) => x.K.CompareTo(y.K));
    Console.WriteLine($"{a[0].K},{a[1].K},{a[2].K}");
  end Main;
end Program;
