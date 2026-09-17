//! case: l3_2_0004
//! doc: stdlib
//! section: 3.2
//! title: UTF8.Name is "utf-8"
//! rule: UTF8's Name is "utf-8".
//! cites: 3.2
//! expect: run
//! stdout: utf-8
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Encoding.UTF8.Name);
  end Main;
end Program;
