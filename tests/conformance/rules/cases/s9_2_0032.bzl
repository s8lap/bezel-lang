//! case: s9_2_0032
//! doc: spec
//! section: 9.2
//! title: a boxed generic record with a declared Equals answers through its Object slot, not by reference
//! rule: "On a record that is **not** auto-hashable, such a call reaches, member by member, the record's own declaration of `Equals`" — "the own-typed member, which is its `Object` slot" ([§9.2]): "a boxed `T` argument compares by it on the box's payload". The record `R<T>` declares `Equals(other: R<T>)` and so is not auto-hashable; `o.Equals(b)` through the `Object`-typed box reaches that declaration, which returns `true` for every argument.
//! cites: 9.2, 8.6
//! expect: run
//! stdout: True
//! issue: 6147
module Conf;

public record R<T>(X: T)
  public method Equals(other: R<T>): bool => true;
  public method GetHashCode(): int => 1;
end R;

public class Program
  public static method Main()
    var a := new R<int>(1);
    var b := new R<int>(2);
    var o: Object := a;
    Console.WriteLine(o.Equals(b));
  end Main;
end Program;
