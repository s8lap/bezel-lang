//! case: s3_4_3607
//! doc: spec
//! section: 3.4
//! title: the qualified System.String names the string type as a static-member root
//! rule: Wherever a type can stand, then, a bare reserved name names the type, and its qualified
//!   spelling — the table's right column, `System.String`, `System.Int32`, reached by §2.7 rule 2 as §2.3
//!   reaches every always-available name — names the same type.
//! cites: 3.4, 2.3, 2.7, 1.4
//! expect: run
//! stdout: True
//! issue: 8116
module Conf;

import System;

public class Program
  public static method Main()
    Console.WriteLine(System.String.IsNullOrWhiteSpace(" "));
  end Main;
end Program;
