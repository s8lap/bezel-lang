//! case: s21_10_0012
//! doc: spec
//! section: 21.10
//! title: a local enum is a §10 enum wholesale, prefix omission included
//! rule: A local `enum` is a §10 enum wholesale: constants, explicit values,
//!   underlying types, methods, interface conformances, and `case` matching
//!   with prefix omission (§10.6) and enum-scrutinee exhaustiveness (§19.6)
//!   inside the scope.
//! cites: 21.10, 10.6, 19.6
//! expect: run
//! stdout: high
module Conf;

public class Program
  public static method Main()
    enum Level
      Low, High;
    end Level;
    var chosen := Level.High;
    Console.WriteLine((case chosen of Low => "low", High => "high"));
  end Main;
end Program;
