//! case: l7_4_0001
//! doc: stdlib
//! section: 7.4
//! title: Directory.Delete(path) resolves via the recursive := false default
//! rule: public static method Delete(path: string, recursive: bool :=
//!   false); // IOException if non-empty and not recursive
//! cites: 7.4
//! expect: accept
module Conf;

import System.IO;

public class Program
  public method Clean(path: string)
    Directory.Delete(path);
  end Clean;
end Program;
