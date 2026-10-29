//! case: s22_5_0060
//! doc: spec
//! section: 22.5
//! title: Array<int>.LastIndexOf with 2 argument(s) is rejected: it declares one parameter
//! rule: Each supplied argument binds **exactly one** parameter
//! cites: 22.5, 22.6, 4.4
//! expect: reject
//! diagnostic: lastindexof
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 1];
    Console.WriteLine(a.LastIndexOf(1, 0));
  end Main;
end Program;
