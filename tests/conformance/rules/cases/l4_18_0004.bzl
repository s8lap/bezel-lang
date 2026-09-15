//! case: l4_18_0004
//! doc: stdlib
//! section: 4.18
//! title: First on an empty SortedSet raises InvalidOperationException
//! rule: pure readonly property First: T; // least; InvalidOperationException
//!   if empty.
//! cites: 4.18
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    try
      var x := ss.First;
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
