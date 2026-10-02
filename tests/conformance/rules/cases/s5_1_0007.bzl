//! case: s5_1_0007
//! doc: spec
//! section: 5.1
//! title: double does not implicitly convert to float
//! rule: Implicit conversions occur only as listed in §5.1's table; double to
//!   float narrows and is not among the table's rows. Twin of s5_1_0006: same
//!   pair, direction reversed.
//! cites: 5.1
//! expect: reject
//! diagnostic: conversion from `double` to `float`
module Conf;

public class Program
  public static method Convert(d: double)
    var f: float := d;
  end Convert;
end Program;
