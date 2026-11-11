//! case: s28_3_11_4701
//! doc: spec
//! section: 28.3.11
//! title: an async method returning a lazy query over its own local array is rejected at the return
//! rule: is rejected at the return site, since the frame would outlive the source; the remedy is to
//!   materialize instead
//! cites: 28.3, 29.4, 30.1
//! expect: reject
//! diagnostic: return
module Conf;

public class Program
  public static async method Evens(): Task<IEnumerable<int>>
    var a: Array<int> := [1, 2, 3];
    return a.Where(lambda(x) => x > 1);
  end Evens;
end Program;
