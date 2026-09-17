//! case: l10_1_0003
//! doc: stdlib
//! section: 10.1
//! title: CString.From(s) is the sanctioned construction route
//! rule: public static method From(s: string): owned CString;
//! cites: 10.1
//! expect: accept
module Conf;

public class Program
  public method Demo()
    using var cs := CString.From("abc");
  end Demo;
end Program;
