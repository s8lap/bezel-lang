//! case: s12_9_0004
//! doc: spec
//! section: 12.9
//! title: a variant assignment whose source has a value-type type argument is rejected
//! rule: What is rejected is the variant assignment of IEnumerable<int> to
//!   IEnumerable<Object>, which would require per-element boxing at every
//!   access. For value-typed T, the only legal assignment of
//!   IEnumerable<T> is to IEnumerable<T> itself or to Object.
//! cites: 12.9
//! expect: reject
//! diagnostic: variant
module Conf;

public class Program
  public static method Test()
    var arr: Array<int> := [1, 2, 3];
    var xs: IEnumerable<int> := arr;
    var ys: IEnumerable<Object> := xs;
  end Test;
end Program;
