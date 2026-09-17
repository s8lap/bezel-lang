//! case: s21_2_0001
//! doc: spec
//! section: 21.2
//! title: an if condition must be bool
//! rule: The condition must be `bool`.
//! cites: 21.2
//! expect: reject
//! diagnostic: bool
module Conf;

public class Program
  public static method Main()
    var count := 5;
    if count then
      Console.WriteLine("nonzero");
    end if;
  end Main;
end Program;
