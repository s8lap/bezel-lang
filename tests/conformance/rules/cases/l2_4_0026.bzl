//! case: l2_4_0026
//! doc: stdlib
//! section: 2.4
//! title: The System-qualified Get is string? too, so binding it to a string local is rejected
//! rule: The reverse requires a null check or `!`.
//! cites: 2.4, 7.1
//! expect: reject
//! diagnostic: nullable but the target type is non-nullable
//! issue: 8096
//! notes: twin of l2_4_0021 (pairs the rejected with its accepting form).
module Conf;

import System;

public class Program
  public static method Main()
    var x: string := System.Environment.Get("BZ_H3_A");
  end Main;
end Program;
