//! case: s22_7_0012
//! doc: spec
//! section: 22.7
//! title: a standard-library builtin gets no implicit parameterless constructor
//! rule: A **standard-library builtin** type is excepted from the implicit
//!   parameterless constructor: its constructor surface is closed by §17.7,
//!   and `Subscription` declares none, so `new Subscription(...)` is a compile
//!   error at any argument list.
//! cites: 22.7, 17.7
//! expect: reject
//! diagnostic: constructor
//! issue: 4364
module Conf;

public class Program
  public method Run()
    using var s := new Subscription();
  end Run;
end Program;
