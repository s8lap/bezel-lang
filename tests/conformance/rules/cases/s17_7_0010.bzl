//! case: s17_7_0010
//! doc: spec
//! section: 17.7
//! title: Guid declares no constructor and admits no new Guid(...)
//! rule: "A builtin declaring **no** constructor — `Guid` ... — admits no
//!   `new T(...)` at any argument list; the diagnostic names the declared
//!   producers — for `Guid`: `Guid.Empty`, `Guid.NewGuid()`, or
//!   `default(Guid)`."
//! cites: 17.7
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public static method Main()
    var g := new Guid();
  end Main;
end Program;
