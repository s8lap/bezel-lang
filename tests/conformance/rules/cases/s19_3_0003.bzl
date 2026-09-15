//! case: s19_3_0003
//! doc: spec
//! section: 19.3
//! title: a relational pattern over a nullable scrutinee is rejected
//! rule: A nullable scrutinee — value or reference — is a compile error at
//!   a relational pattern: ordering admits no absent participant, and
//!   pattern position performs no narrowing.
//! cites: 19.3
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var n: int? := 80;
    case n of
      > 70 => Console.WriteLine("high");,
      _    => Console.WriteLine("low");
    end case;
  end Main;
end Program;
