//! case: l7_3_0001
//! doc: stdlib
//! section: 7.3
//! title: File.WriteAllBytes accepts a ReadOnlySpan<byte> for the zero-copy serializer path
//! rule: public static method WriteAllBytes(path: string, contents:
//!   ReadOnlySpan<byte>); // v0.165 (#1358): serializer-output path,
//!   zero-copy
//! cites: 7.3
//! expect: accept
module Conf;

import System.IO;

public class Program
  public method Save(path: string, contents: ReadOnlySpan<byte>)
    File.WriteAllBytes(path, contents);
  end Save;
end Program;
