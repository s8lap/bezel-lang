//! case: l3_3_0004
//! doc: stdlib
//! section: 3.3
//! title: the indexer raises IndexOutOfRangeException outside 0..Length
//! rule: span[i] throws IndexOutOfRangeException when i < 0 or i >= Length.
//! cites: 3.3
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var span := "abc".AsSpan();
    try
      _ := span[10];
    catch
      IndexOutOfRangeException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
