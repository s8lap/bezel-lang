//! case: l9_8_0002
//! doc: stdlib
//! section: 9.8
//! title: the `in` operator invokes Range<T>.Contains
//! rule: This is the operation invoked by the in operator.
//! cites: 9.8
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(5 in 0..10);
  end Main;
end Program;
