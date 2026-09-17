//! case: s4_6_0003
//! doc: spec
//! section: 4.6
//! title: out-of-range string indexing raises IndexOutOfRangeException
//! rule: s[i] ... Out-of-range raises IndexOutOfRangeException.
//! cites: 4.6
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      var c := "hi"[5];
      Console.WriteLine("no");
    catch
      var ex: IndexOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
