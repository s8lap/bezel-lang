//! case: s7_5_4705
//! doc: spec
//! section: 7.5
//! title: ?.Sum() on a null interface receiver yields null and the ?? fallback is taken
//! rule: if it is null the access does not occur and nothing to its right in the chain is evaluated
//! cites: 5, 7.7
//! expect: run
//! stdout: -1
module Conf;

public class Program
  public static method Main()
    var nl: IList<int>? := null;
    Console.WriteLine(nl?.Sum() ?? -1);
  end Main;
end Program;
