//! case: s15_2_0001
//! doc: spec
//! section: 15.2
//! title: a heap array used at fixed size keeps its literal-constructed Count
//! rule: Users who want fixed-size discipline simply do not call resizing
//!   methods; the array's `Count` matches its construction-time capacity
//!   throughout its lifetime (§15.2).
//! cites: 15.2
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    using var snapshot: Array<int> := [1, 2, 3];
    Console.WriteLine(snapshot.Count);
  end Main;
end Program;
