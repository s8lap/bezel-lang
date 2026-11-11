//! case: s15_4_4501
//! doc: spec
//! section: 15.4
//! title: a collection literal passed to an IEnumerable<string> parameter has no
//!   admitted target type
//! rule: A collection literal has no standalone type. It is well-typed only in a
//!   context that provides the target type
//! cites: 22.6
//! expect: reject
//! diagnostic: collection literal
//! issue: 8390
module Conf;
import System.IO;

class C
  public static method Exercise()
    File.WriteAllLines("scratch_lines.txt", ["a", "b"]);
  end Exercise;
end C;
