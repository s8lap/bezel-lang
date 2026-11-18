//! case: s7_5_7109
//! doc: spec
//! section: 7.5
//! title: `?.First` of a LinkedList<int?> with a stored null first element reads null, as a null receiver does
//! rule: The result type is always nullable
//! cites: 7.5, 7.7
//! expect: run
//! output: -9
//!   -9
module Conf;
public class Program
  public static method A(m: LinkedList<int?>?)
    var x := m?.First;
    Console.WriteLine(x ?? -9);
  end A;
  public static method Main()
    A(null);
    using var li := new LinkedList<int?>();
    _ := li.AddLast(null);
    A(li);
  end Main;
end Program;
