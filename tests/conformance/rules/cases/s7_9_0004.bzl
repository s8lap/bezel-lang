//! case: s7_9_0004
//! doc: spec
//! section: 7.9
//! title: reading a non-nullable local before it is assigned is rejected
//! rule: Definite-assignment analysis applies the same rule to non-nullable
//!   locals: a non-nullable local must be assigned before it is read.
//! expect: reject
//! diagnostic: before its first read
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w: Widget;
    var w2 := w;
  end Main;
end Program;
