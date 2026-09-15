//! case: s20_0009
//! doc: spec
//! section: 20
//! title: an all-null arm set has no type and is a compile error
//! rule: "an all-`null` arm set has no type and is a compile error."
//! cites: 20
//! expect: reject
//! diagnostic: no type
module Conf;

public class Program
  public static method Main()
    var v := (if true then null else null);
  end Main;
end Program;
