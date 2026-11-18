//! case: l4_1_6801
//! doc: stdlib
//! section: 4.1
//! title: the read of the property IsReadOnly through an IList receiver is admitted and yields false
//! rule: `IsReadOnly` is `false` for every standard collection implementing `ICollection<T>`
//! cites: 4.1, 26.5, 27.7
//! expect: run
//! stdout: False
//! notes: Accepting twin of s26_5_6801 (the `()` removed); the member is declared on ICollection<T>, which IList<T> extends.
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var c: IList<int> := a;
    Console.WriteLine(c.IsReadOnly);
  end Main;
end Program;
