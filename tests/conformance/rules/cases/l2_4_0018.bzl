//! case: l2_4_0018
//! doc: stdlib
//! section: 2.4
//! title: a call to GetInt supplying two arguments matches no declared form
//! rule: public static method GetInt(name: string): int?;    // convenience tier: null when the variable is
//! cites: 22.5
//! expect: reject
//! diagnostic: GetInt
//! issue: 7949
//! notes: Twin: the one-argument GetInt rows. Spec 22.5: every parameter must be bound and each supplied argument binds one parameter, so a second argument has none.
module Conf;

public class Program
  public static method Main()
    var n := Environment.GetInt("BZ_H2_X", 1);
  end Main;
end Program;
