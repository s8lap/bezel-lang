//! case: l10_3_0004
//! doc: stdlib
//! section: 10.3
//! title: RawBytes is not sendable and cannot cross a domain boundary
//! rule: RawBytes is a class and not sendable (language spec §30.14's
//!   class rule — not sealed), so a FromForeign view never crosses a
//!   domain boundary; stated here so the invariant is explicit.
//! cites: 10.3, 30.14
//! expect: reject
//! diagnostic: sendable
module Conf;

import System.Threading;

public class Program
  public static method UseBytes(owned b: RawBytes)
    Console.WriteLine(b.Length);
  end UseBytes;

  public method Run()
    var src := new Array<byte>(1, 0);
    var rb := RawBytes.From(src);
    using var t := Domain.Spawn(Program.UseBytes, move rb);
  end Run;
end Program;
