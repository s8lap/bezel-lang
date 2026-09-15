//! case: s29_12_0001
//! doc: spec
//! section: 29.12
//! title: adding to a collection while iterating it raises InvalidOperationException on the next advance
//! rule: Modifying a collection while iterating it invalidates the active
//!   enumerator. Standard collections detect this and throw
//!   `InvalidOperationException` on the next `MoveNext()`.
//! cites: 29.12
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    try
      for var x in xs do
        xs.Add(99);
      end;
    catch
      InvalidOperationException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
