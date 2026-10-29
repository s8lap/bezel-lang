//! case: l5_0026
//! doc: stdlib
//! section: 5
//! title: Contains reaches the instance member on IList and the extension on IReadOnlyList
//! rule: ICollection<T> (and so IList<T>) declares Contains(item: T), which an extension cannot shadow; IReadOnlyList<T> declares none, so the section 5 Contains(value: T) extension (T: hashable) is reached. Both answer true for 3 in [5, 3, 8, 1].
//! cites: 4.1, 5, 26.5
//! expect: run
//! output: True
//!   True
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(r.Contains(3));
    Console.WriteLine(l.Contains(3));
  end Main;
end Program;
