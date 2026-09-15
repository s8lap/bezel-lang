//! case: l3_2_0005
//! doc: stdlib
//! section: 3.2
//! title: UTF8.GetBytes never emits a byte-order mark
//! rule: GetBytes never emits a byte-order mark.
//! cites: 3.2
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    var bytes := Encoding.UTF8.GetBytes("hi");
    Console.WriteLine(bytes.Count);
  end Main;
end Program;
