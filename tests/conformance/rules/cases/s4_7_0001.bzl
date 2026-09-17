//! case: s4_7_0001
//! doc: spec
//! section: 4.7
//! title: a method with no declared return type has the unit-type result
//! rule: () is the type with exactly one value, also written (). It is the
//!   return type of methods declared without an explicit return type.
//! cites: 4.7
//! expect: accept
module Conf;

public class Worker
  public static method Run()
  end Run;
end Worker;
