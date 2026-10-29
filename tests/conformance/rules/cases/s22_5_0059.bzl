//! case: s22_5_0059
//! doc: spec
//! section: 22.5
//! title: Array<int>.LastIndexOf with 0 argument(s) is rejected: it declares one parameter
//! rule: Every parameter that declares no default must be bound
//! cites: 22.5, 22.6, 4.4
//! expect: reject
//! diagnostic: lastindexof
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 1];
    Console.WriteLine(a.LastIndexOf());
  end Main;
end Program;
