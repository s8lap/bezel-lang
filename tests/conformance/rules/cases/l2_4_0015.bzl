//! case: l2_4_0015
//! doc: stdlib
//! section: 2.4
//! title: Get returns the same value when returned through a nullable method result
//! rule: public static method Get(name: string): string?;    // null when the variable is absent
//! expect: run
//! stdout: value
//! issue: 7950
//! notes: Returned shape.
module Conf;

public class Program
  private static method Mk(): string?
    return Environment.Get("BZ_H2_GET_C");
  end Mk;

  public static method Main()
    Environment.Set("BZ_H2_GET_C", "value");
    var c := Mk();
    Console.WriteLine(c ?? "none");
  end Main;
end Program;
