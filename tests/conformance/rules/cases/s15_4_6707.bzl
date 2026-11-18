//! case: s15_4_6707
//! doc: spec
//! section: 15.4
//! title: a collection literal at a user extension's Array parameter is well-typed
//! rule: - `Array<T>` (heap-borrowing) and `OwningArray<T>` (heap-owning).
//! cites: 15.4, 26.5.1
//! expect: run
//! stdout: 82
module Conf;

extension Array<int>
  public method Concat(o: Array<int>): int => 82;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1];
    Console.WriteLine(a.Concat([2, 3]));
  end Main;
end Program;
