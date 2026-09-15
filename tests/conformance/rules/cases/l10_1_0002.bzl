//! case: l10_1_0002
//! doc: stdlib
//! section: 10.1
//! title: CString has no public constructor — instances come from its factory methods
//! rule: public class CString // No public constructor; obtained via
//!   factory methods.
//! cites: 10.1
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public method Demo()
    var cs := new CString();
  end Demo;
end Program;
