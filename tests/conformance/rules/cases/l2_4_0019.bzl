//! case: l2_4_0019
//! doc: stdlib
//! section: 2.4
//! title: a call to GetBool supplying no argument matches no declared form
//! rule: public static method GetBool(name: string): bool?;  //   absent OR malformed — deliberately conflated
//! cites: 22.5
//! expect: reject
//! diagnostic: GetBool
//! issue: 7949
//! notes: Spec 22.5: a parameter that declares no default must be bound; name has none.
module Conf;

public class Program
  public static method Main()
    var b := Environment.GetBool();
  end Main;
end Program;
