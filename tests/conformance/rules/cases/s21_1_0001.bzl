//! case: s21_1_0001
//! doc: spec
//! section: 21.1
//! title: a defaultable local declared without an initializer holds that type's default
//! rule: A local of non-defaultable type must be assigned before it is read; a
//!   local of defaultable type declared without an initializer holds that
//!   type's default from its declaration — `var balance: decimal;` is `0m`.
//! cites: 21.1, 7.9, 17.11
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var balance: decimal;
    Console.WriteLine(balance = 0m);
  end Main;
end Program;
