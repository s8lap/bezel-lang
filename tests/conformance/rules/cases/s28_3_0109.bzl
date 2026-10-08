//! case: s28_3_0109
//! doc: spec
//! section: 28.3
//! title: a frame built in a generic body over a forwarded T parameter may be returned
//! rule: A non-iterator method may therefore compose and return such a frame ...
//!   provided every borrow the returned frame carries is sourced from the
//!   method's own parameters or receiver
//! cites: 28.3, 29.4
//! expect: accept
module Conf;

public class Seq
  public static method Once<T>(x: T): IEnumerable<T>
    yield return x;
  end Once;

  public static method Forward<T>(a: T): IEnumerable<T> => Once<T>(a);
end Seq;
