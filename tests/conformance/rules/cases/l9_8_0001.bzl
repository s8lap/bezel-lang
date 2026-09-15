//! case: l9_8_0001
//! doc: stdlib
//! section: 9.8
//! title: GetEnumerator on an open-ended range raises InvalidOperationException
//! rule: Calling GetEnumerator on an open-ended range (IsOpenEnded is
//!   true) raises InvalidOperationException.
//! cites: 9.8
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var r := 5..;
    try
      for var x in r do
      end;
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
