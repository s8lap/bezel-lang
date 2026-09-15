//! case: s29_4_0001
//! doc: spec
//! section: 29.4
//! title: an iterator method's body does not run until MoveNext is first called
//! rule: An iterator method's body does not run when the method is called.
//!   Calling produces an `IEnumerable<T>` (or `IEnumerator<T>`); the body
//!   runs incrementally as the consumer calls `MoveNext()`.
//! cites: 29.4
//! expect: run
//! stdout: after-call
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    Console.WriteLine("inside");
    yield return 1;
  end Gen;

  public static method Main()
    var it := Gen();
    Console.WriteLine("after-call");
    for var x in it do
      _ := x;
    end;
  end Main;
end Program;
