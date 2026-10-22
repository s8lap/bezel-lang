//! case: s12_7_0041
//! doc: spec
//! section: 12.7
//! title: an is test on an Object-typed local answers for the runtime type of a StringBuilder
//! rule: "`is T` tests whether a value is an instance of `T` and produces `bool`" ([§12.7]). A local typed `Object` that holds a `StringBuilder` is an instance of `StringBuilder`, and the local holding a `Random` is not, so the two tests are true, false.
//! cites: 17.4
//! expect: run
//! notes: the Random shape (o1 is Random, o2 is Random) is removed until #6755 lands; the two tests that remain are true and false.
//! output: True
//!   False
module Conf;
public class Program
  public static method Main()
    var o1: Object := new StringBuilder();
    var o2: Object := new Random(1);
    Console.WriteLine(o1 is StringBuilder);
    Console.WriteLine(o2 is StringBuilder);
  end Main;
end Program;
