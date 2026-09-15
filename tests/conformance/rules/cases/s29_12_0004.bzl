//! case: s29_12_0004
//! doc: spec
//! section: 29.12
//! title: direct-indexed loop elision still raises InvalidOperationException on modification
//! rule: The §29.2 protocol-elision optimization must preserve these
//!   faults: a `for` loop the compiler lowers to direct indexing still
//!   raises `InvalidOperationException` at the iteration boundary following
//!   a structural modification, exactly as the enumerator path would.
//! cites: 29.12, 29.2
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3, 4, 5];
    try
      for var x in xs do
        xs.RemoveAt(0);
      end;
    catch
      InvalidOperationException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
