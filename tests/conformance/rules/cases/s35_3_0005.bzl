//! case: s35_3_0005
//! doc: spec
//! section: 35.3
//! title: generic choice construction with explicit type arguments is admitted
//! rule: For a generic choice the type arguments are written at the construction:
//!   Result<int, string>.Ok(value).
//! expect: accept
module Conf;

public class Program
  public static method Use()
    var r := Result<int, string>.Ok(4);
  end Use;
end Program;
