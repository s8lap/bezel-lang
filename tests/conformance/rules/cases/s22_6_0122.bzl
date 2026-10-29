//! case: s22_6_0122
//! doc: spec
//! section: 22.6
//! title: the null literal bound by name to DateOnly's non-nullable day parameter is rejected
//! rule: it is applicable at every nullable parameter and at no non-nullable one
//! cites: 22.6
//! expect: reject
//! diagnostic: non-nullable
module Conf;

public class Program
  public static method Build(): DateOnly
    return new DateOnly(2020, 1, day := null);
  end Build;
end Program;
