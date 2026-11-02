//! case: s22_6_0126
//! doc: spec
//! section: 22.6
//! title: a narrowed nullable argument is of its non-nullable type at the call
//! rule: **The argument's static type** is its type at the call: a read of a binding [§7.4](#sec-member-access-on-nullable-values) has narrowed non-null is of its non-nullable type, as [§7.11](#sec-lifted-operators) reads it
//! cites: 22.6, 7.4, 5.1
//! expect: run
//! output: 2
//! issue: 7957
module Conf;
import System.Serialization;

public class Program
  public static method Main(): int
    var n: int? := 2;
    var o := new JsonOptions(true, n, false, false, 3);
    Console.WriteLine(o.IndentWidth);
    return 0;
  end Main;
end Program;
