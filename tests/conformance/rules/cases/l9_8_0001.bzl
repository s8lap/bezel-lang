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
    var r: Range<int> := 5..;
    try
      using var e := r.GetEnumerator();
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
