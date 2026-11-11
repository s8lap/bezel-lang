//! case: s5_1_4302
//! doc: spec
//! section: 5.1
//! title: a Guid does not convert to IEquatable<DateTime>
//! rule: a type to an interface it implements or conforms to
//! cites: 5.1, 2.6, 2.8
//! expect: reject
//! diagnostic: has a type that is not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var g: IEquatable<DateTime> := Guid.Empty;
  end Main;
end Program;
