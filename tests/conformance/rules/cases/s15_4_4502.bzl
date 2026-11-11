//! case: s15_4_4502
//! doc: spec
//! section: 15.4
//! title: a literal bound to an Array<string> local is admitted and the local passes
//!   to an IEnumerable<string>
//! rule: A collection literal has no standalone type. It is well-typed only in a
//!   context that provides the target type
//! cites: 22.6
//! expect: accept
//! issue: 8390
module Conf;
import System.IO;

class C
  public static method Exercise()
    var xs: Array<string> := ["a", "b"];
    File.WriteAllLines("scratch_lines.txt", xs);
  end Exercise;
end C;
