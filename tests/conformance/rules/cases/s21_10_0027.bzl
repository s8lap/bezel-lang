//! case: s21_10_0027
//! doc: spec
//! section: 21.10
//! title: a method body inside a local record may not name the enclosing method's type parameter
//! rule: A local type may not reference an enclosing generic parameter (an
//!   enclosing method's or class's `T`) — a compile error naming the rule.
//! cites: 21.10
//! expect: reject
//! diagnostic: enclosing generic parameter
module Conf;

public static class Util
  public static method Go<T>(t: T): int
    record P(A: int)
      public method Run<U>(x: U): int
        var z: T := default(T);
        return A;
      end Run;
    end P;
    var p := new P(1);
    return p.Run(2);
  end Go;
end Util;
