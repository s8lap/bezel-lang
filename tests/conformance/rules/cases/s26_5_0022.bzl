//! case: s26_5_0022
//! doc: spec
//! section: 26.5
//! title: an extension named like the string Length property does not reach a call of that name
//! rule: "Instance members win — by name. If `x`'s declared type (or any base type)
//!   *declares* any instance member named `Foo` — of any arity, applicability, or
//!   accessibility — instance resolution is committed and extension blocks are
//!   **never consulted** for that name on that receiver. If the committed instance
//!   lookup then fails ..., the call is a compile error citing the instance member(s);
//!   it does not fall back to an extension." (§26.5, v0.350.) The string type declares
//!   the instance property `Length`, so the extension method `Length()` is never
//!   consulted for `s.Length()`, and that call is a compile error.
//! cites: 26.5
//! expect: reject
//! diagnostic: is a property of
module Conf;

import System;

extension string
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    var s := "abc";
    Console.WriteLine(s.Length());
  end Main;
end Program;
