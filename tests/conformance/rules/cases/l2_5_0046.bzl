//! case: l2_5_0046
//! doc: stdlib
//! section: 2.5
//! title: an ArgumentException catch arm and an Exception catch arm both receive the out-of-range DateTime
//! rule: so that an `ArgumentException` catch arm covers the whole argument-validation family
//! cites: 2.5
//! expect: run
//! output: AE
//!   E
module Conf;

public class Program
  public static method Main()
    try
      var d := new DateTime(2020, 1, 32);
    catch var e: ArgumentException => Console.WriteLine("AE"); end;
    try
      var d := new DateTime(2020, 1, 32);
    catch var e: Exception => Console.WriteLine("E"); end;
  end Main;
end Program;
