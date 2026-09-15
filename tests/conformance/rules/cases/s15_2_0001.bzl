//! case: s15_2_0001
//! doc: spec
//! section: 15.2
//! title: a heap array used at fixed size keeps its literal-constructed Count
//! rule: Users who want fixed-size discipline simply do not call resizing
//!   methods; the array's `Count` matches its construction-time capacity
//!   throughout its lifetime (§15.2).
//! cites: 15.2
//! expect: run
//! output: 3
//!   9
module Conf;

public class Program
  public static method Main()
    using var snapshot: Array<int> := [1, 2, 3];
    snapshot[0] := 9;
    Console.WriteLine(snapshot.Count);
    Console.WriteLine(snapshot[0]);
  end Main;
end Program;
