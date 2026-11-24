//! case: s9_7_8603
//! doc: spec
//! section: 9.7
//! title: a closure returned by a lambda over its string? parameter reads it after the lambda returned
//! rule: runtime-managed values have unbounded lifetime by construction, so no borrow-lifetime rules apply
//! cites: 9.7, 28.9, 27.5
//! expect: run
//! stdout: 1090
//! notes: Harvested from the #9069 review (probe p23_lambda_param_nstr). mk is a lambda whose string? parameter p is captured by the closure it returns. Each f() reads "fr-{j}" of length 3+digits(j); sum = 1090.
module Conf;
import System;
public class Program
  public static method Main(): int
    var t := 0;
    var mk := lambda(p: string?): () -> int => lambda(): int => (p ?? "").Length;
    for var j in 0..<200 do
      var f := mk($"fr-{j}");
      t := t + f();
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
