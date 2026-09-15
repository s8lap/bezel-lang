//! case: l4_17_0006
//! doc: stdlib
//! section: 4.17
//! title: structurally modifying a SortedDictionary during iteration raises InvalidOperationException
//! rule: Iteration is O(n) in ascending key order, one live pass at a time;
//!   structural modification during iteration raises
//!   InvalidOperationException.
//! cites: 4.17, 29.12
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, string>();
    sd.Add(1, "a");
    sd.Add(2, "b");
    try
      for var e in sd do
        sd.Add(3, "c");
      end;
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
