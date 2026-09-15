//! case: s31_2_1_0009
//! doc: spec
//! section: 31.2.1
//! title: a generic type parameter bounded by Exception matches the substituted type
//! rule: A generic type parameter is admissible as an arm's type when it
//!   carries an `Exception`-derived class bound; it matches exactly as the
//!   substituted type argument would.
//! cites: 31.2.1
//! expect: run
//! stdout: caught
module Conf;

public class Classifier
  public static method Classify<TEx>(action: () -> ()): string where TEx: Exception
    try
      action();
      return "no throw";
    catch
      var ex: TEx => return "caught",
    end try;
  end Classify;
end Classifier;

public class Program
  public static method Main()
    Console.WriteLine(Classifier.Classify<ArgumentException>(lambda() => throw new ArgumentException("x")));
  end Main;
end Program;
