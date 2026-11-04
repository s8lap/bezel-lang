//! case: s2_7_0808
//! doc: spec
//! section: 2.7
//! title: a user class named Encoding in the current module takes the bare name before the built-in Encoding
//! rule: If `A` resolves to a *type*, the rest of the chain resolves against that type's members
//! cites: 2.7, 2.3
//! expect: run
//! stdout: UCLSENC
//! issue: 8126
//! notes: the current module's declarations precede imported and always-available bare names in rule 1's order.
module Conf;

public class Encoding
  public static property UTF8: string => "UCLSENC";
end Encoding;

public class Program
  public static method Main()
    Console.WriteLine(Encoding.UTF8);
  end Main;
end Program;
