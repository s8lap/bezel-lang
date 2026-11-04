//! case: s17_7_0014
//! doc: spec
//! section: 17.7
//! title: BinarySerializer declares only the parameterless constructor; one argument is rejected
//! rule: `new T(...)` resolves only against a declared constructor, never against a
//!   compiler-supplied fallback
//! cites: 17.7, 22.5
//! expect: reject
//! diagnostic: constructor
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    using var w := new BinarySerializer(1);
  end Main;
end Program;
