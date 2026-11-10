// Probe f20 (proposal 0211 §4.4): the straddling types on r40's own library.
//   (1) `OverflowException` is raised for out-of-range input to `Parse` and for an
//       arithmetic overflow; no arm on today's types takes one without the other.
//   (2) `FormatException` is raised for malformed input and for a format
//       specifier the program got wrong.
// A boundary that lists today's failure types by hand therefore either lets bad
// input through as a bug (omit `OverflowException`) or takes an arithmetic bug as
// bad input (include it).
module F20;

public class Program
  static method Raise(which: int)
    if which = 0 then
      _ := int.Parse("99999999999");            // bad input: out of range
    elif which = 1 then
      var big := int.MaxValue;
      Console.WriteLine(big + which);            // a bug: arithmetic overflow
    elif which = 2 then
      _ := int.Parse("12x");                    // bad input: malformed
    elif which = 3 then
      Console.WriteLine(42.ToString("Q9"));     // a bug: no such specifier
    end if;
  end Raise;

  public static method Main()
    for var i in 0..3 do
      try
        Program.Raise(i);
      catch
        var ex: OverflowException => Console.WriteLine($"case {i}: OverflowException arm"),
        var ex: FormatException => Console.WriteLine($"case {i}: FormatException arm"),
      end try;
    end for;
  end Main;
end Program;
