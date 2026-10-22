//! case: s5_1_0124
//! doc: spec
//! section: 5.1
//! title: a reference element converts to its nullable view inside a covariant interface
//! rule: "`S?` implicitly converts to `T?` whenever `S` implicitly converts to `T`" ([§5.1]). For a reference type the conversion is "the same reference viewed as nullable" ([§5.1]): the non-null ArgumentException element array converts to IEnumerable<Exception?>, and the nullable-element array with one null converts the same way, so the count of null elements is 1 and the empty array adds nothing.
//! cites: 7.1, 12.9
//! expect: run
//! output: 1
module Conf;
public class Program
  public static method Main(): int
    using var a := new Array<ArgumentException?>();
    a.Add(null);
    var e: IEnumerable<Exception?> := a;
    var n := 0;
    for var v in e do if v = null then n := n + 1; end; end;
    using var b := new Array<ArgumentException>();
    var f: IEnumerable<Exception?> := b;
    for var v in f do n := n + 10; end;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
