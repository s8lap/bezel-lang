//! case: l7_2_0001
//! doc: stdlib
//! section: 7.2
//! title: FileStream(path, mode) resolves via the FileAccess.ReadWrite default
//! rule: access defaults to FileAccess.ReadWrite: new FileStream(path,
//!   mode) is new FileStream(path, mode, FileAccess.ReadWrite).
//! cites: 7.2
//! expect: accept
module Conf;

import System.IO;

public class Program
  public method Open(path: string)
    using var fs := new FileStream(path, FileMode.Open);
  end Open;
end Program;
