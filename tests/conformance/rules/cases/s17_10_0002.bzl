//! case: s17_10_0002
//! doc: spec
//! section: 17.10
//! title: typeof(int?) is rejected — nullable forms are not accepted
//! rule: "Nullable forms are not accepted (`typeof(int?)` is a compile
//!   error)."
//! cites: 17.10
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var t: Type := typeof(int?);
  end Main;
end Program;
