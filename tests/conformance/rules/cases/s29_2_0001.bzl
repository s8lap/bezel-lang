//! case: s29_2_0001
//! doc: spec
//! section: 29.2
//! title: the for variable over value-typed elements is a copy
//! rule: The `for` desugaring binds `var x := __enumerator.Current;`, and for a
//!   copy slot — a value-typed element of a borrowing collection — the element
//!   operation *indexer-get / default iteration* is "copy out" (§28.11's
//!   element slot policy), so writing to the loop variable does not reach the
//!   collection.
//! cites: 29.2, 28.11
//! expect: run
//! stdout: 6
module Conf;

public class Program
  public static method Main()
    var arr: Array<int> := [1, 2, 3];
    for var x in arr do
      x := x + 100;
    end;
    var total := 0;
    for var y in arr do
      total := total + y;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
