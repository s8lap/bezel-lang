//! case: l4_17_0005
//! doc: stdlib
//! section: 4.17
//! title: the indexer get raises KeyNotFoundException on an absent key
//! rule: property [key: K]: V; // get: KeyNotFoundException if absent.
//! cites: 4.17
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, string>();
    try
      var v := sd[5];
    catch
      KeyNotFoundException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
