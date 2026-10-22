//! case: s12_2_0004
//! doc: spec
//! section: 12.2
//! title: a nested type named ToString hides the inherited Object.ToString and is rejected
//! rule: "Declaring a member in a derived class with the same name as"
//! cites: 11.6, 12.2
//! expect: reject
//! diagnostic: hides the inherited `Object.ToString`
module Conf;

public class Outer
  public class ToString
  end ToString;
end Outer;

public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
