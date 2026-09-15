//! case: s21_1_0001
//! doc: spec
//! section: 21.1
//! title: a non-nullable local must be assigned before it is read
//! rule: A non-nullable local must be assigned before it is read.
//! cites: 21.1, 7.9
//! expect: reject
//! diagnostic: assign
module Conf;

public class Program
  public static method Main()
    var balance: decimal;
    Console.WriteLine(balance.ToString());
  end Main;
end Program;
