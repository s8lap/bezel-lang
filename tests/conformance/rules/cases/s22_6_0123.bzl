//! case: s22_6_0123
//! doc: spec
//! section: 22.6
//! title: the null literal bound by name to DateTime's non-nullable hour parameter is rejected
//! rule: it is applicable at every nullable parameter and at no non-nullable one
//! cites: 22.6
//! expect: reject
//! diagnostic: non-nullable
module Conf;

public class Program
  public static method Build(): DateTime
    return new DateTime(2020, 1, 2, hour := null, minute := 1, second := 1);
  end Build;
end Program;
