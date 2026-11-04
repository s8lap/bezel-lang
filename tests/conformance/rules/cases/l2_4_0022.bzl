//! case: l2_4_0022
//! doc: stdlib
//! section: 2.4
//! title: Get's result is string?, so Console.WriteLine of it is rejected
//! rule: by the lifting rule above it converts at most to `Object?`, never to
//!   `Object`
//! cites: 2.1, 2.4, 5.1, 7.1
//! expect: reject
//! diagnostic: nullable but the target type is non-nullable
//! issue: 8096
//! notes: twin of l2_4_0032 (pairs the rejected with its accepting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Environment.Get("BZ_H3_A"));
  end Main;
end Program;
