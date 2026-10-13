// RUN: %not %bezelc --checked-disposal --unchecked-disposal -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc --unchecked-disposal --checked-disposal -c %s 2>&1 | FileCheck %s
// fix #4602 (proposal 0128 §9 Q1, recommended: reject both-given; changelog
// v0.307 "both switches together are a usage error"): giving both disposal-mode
// switches is a usage error with rc 2, in either argument order.
// CHECK: bezelc: error: --checked-disposal and --unchecked-disposal cannot be given together (a build selects one disposal-checking mode, §28.2)
module Conf;

public class Program
  public static method Main()
  end Main;
end Program;
