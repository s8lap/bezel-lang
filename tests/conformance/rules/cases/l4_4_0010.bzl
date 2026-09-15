//! case: l4_4_0010
//! doc: stdlib
//! section: 4.4
//! title: structurally mutating an Array<T> inside its own sort comparer raises
//! rule: Structurally mutating a collection from inside its own sort
//!   comparer raises InvalidOperationException (the loud-failure
//!   discipline, extended from enumerator invalidation to sort callbacks).
//! cites: 4.4, 29.12
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [3, 1, 2];
    try
      arr.Sort(lambda(a: int, b: int): int => do
        arr.Add(9);
        return a - b;
      end);
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
