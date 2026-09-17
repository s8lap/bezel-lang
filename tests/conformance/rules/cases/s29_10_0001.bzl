//! case: s29_10_0001
//! doc: spec
//! section: 29.10
//! title: an iterator method with only yield break is the canonical empty iterator
//! rule: An iterator method with only `yield break;` is the canonical empty
//!   iterator.
//! cites: 29.10
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method Empty(): IEnumerable<int>
    yield break;
  end Empty;

  public static method Main()
    for var x in Empty() do
      Console.WriteLine("unexpected");
    end;
    Console.WriteLine("done");
  end Main;
end Program;
