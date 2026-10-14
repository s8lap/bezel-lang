//! case: s22_3_0051
//! doc: spec
//! section: 22.3
//! title: a ref parameter whose type is a bare type parameter is not rejected at its declaration
//! rule: A `ref` parameter whose declared type is or contains a type parameter is not rejected at its declaration; at a substitution that makes that type a move-only value type, no argument binds it.
//! cites: 22.3
//! expect: accept
//! issue: 5566
module Conf;

public class U
  public static method Put<T>(ref x: T, y: T): int
    x := y;
    return 1;
  end Put;
end U;
