//! case: s7_1_0009
//! doc: spec
//! section: 7.1
//! title: a nullable int argument bound by name at Math.Min is rejected
//! rule: `string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.
//! cites: 2.2, 14.2, 22.5
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: Named spelling: stdlib 2.2 names the parameters a and b; spec 22.5 binds each named argument to one parameter, so the nullable value reaches parameter a.
module Conf;

public class Program
  public static method Pick(d: int?): int
    return Math.Min(a := d, b := 3);
  end Pick;

  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
