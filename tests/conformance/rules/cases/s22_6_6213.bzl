//! case: s22_6_6213
//! doc: spec
//! section: 22.6
//! title: on an overloaded generic name an un-annotated lambda is typed at
//!   the arity-unique candidate
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 22.6, 14.5
//! expect: run
//! stdout: 3
module Conf;

public static class Seq
  public static method M<T, K>(x: T, key: (T) -> K): K
    return key(x);
  end M;

  public static method M(a: int, b: int, c: int, d: int): int
    return a + b + c + d;
  end M;
end Seq;

public class Program
  public static method Main()
    Console.WriteLine(Seq.M("ann", lambda(v) => v.Length));
  end Main;
end Program;
