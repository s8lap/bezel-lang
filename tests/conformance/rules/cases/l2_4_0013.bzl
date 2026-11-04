//! case: l2_4_0013
//! doc: stdlib
//! section: 2.4
//! title: Get returns the value of a variable that was set
//! rule: public static method Get(name: string): string?;    // null when the variable is absent
//! expect: run
//! stdout: value
//! issue: 7950
//! notes: Bound-local shape of the #7950 discharge fix. The case pins the value; leaks are outside the corpus's observation.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_GET_A", "value");
    var a := Environment.Get("BZ_H2_GET_A");
    Console.WriteLine(a ?? "none");
  end Main;
end Program;
