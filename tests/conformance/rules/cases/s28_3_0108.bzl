//! case: s28_3_0108
//! doc: spec
//! section: 28.3
//! title: a frame built in a generic body over its own T-typed local may not be returned
//! rule: A returned frame carrying a borrow of anything else — a scope-bound
//!   local, a `using var`, a temporary — is rejected at the return site,
//!   since the frame would outlive the source ... In a generic body an
//!   argument whose type is a type parameter, or whose borrow-bearing or
//!   holds-borrows classification turns on one, therefore counts what it
//!   contributes at every substitution its constraints admit.
//! cites: 28.3, 29.4
//! expect: reject
//! diagnostic: outlive
//! issue: 5624
module Conf;

public class Seq
  public static method Once<T>(x: T): IEnumerable<T>
    yield return x;
  end Once;

  public static method Fresh<T>(): IEnumerable<T> where T: new()
    using var t := new T();
    return Once<T>(t);
  end Fresh;
end Seq;
