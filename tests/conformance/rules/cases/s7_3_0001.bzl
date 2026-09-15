//! case: s7_3_0001
//! doc: spec
//! section: 7.3
//! title: assigning null to a non-nullable local is rejected
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! expect: reject
//! diagnostic: non-nullable
module Conf;

public class Program
  public static method Main()
    var name: string := null;
  end Main;
end Program;
