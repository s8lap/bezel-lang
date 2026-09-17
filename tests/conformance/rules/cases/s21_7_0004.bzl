//! case: s21_7_0004
//! doc: spec
//! section: 21.7
//! title: return inside a finally block is a compile error
//! rule: "`return` inside a `finally` block is a compile error."
//! cites: 21.7, 31.2
//! expect: reject
//! diagnostic: finally
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine("try");
    finally
      return;
    end try;
  end Main;
end Program;
