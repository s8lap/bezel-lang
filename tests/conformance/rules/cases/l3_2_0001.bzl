//! case: l3_2_0001
//! doc: stdlib
//! section: 3.2
//! title: UTF8 round-trips GetBytes and GetString
//! rule: GetBytes(s: string) / GetString(bytes) form a round trip for
//!   well-formed input.
//! cites: 3.2
//! expect: run
//! stdout: hi
module Conf;

public class Program
  public static method Main()
    var bytes := Encoding.UTF8.GetBytes("hi");
    Console.WriteLine(Encoding.UTF8.GetString(bytes));
  end Main;
end Program;
