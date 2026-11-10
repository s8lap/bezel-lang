// Probe p12: a call-site "to null" stance written as a LIBRARY helper today,
// with no new syntax. Two versions: one that names the failure it expects
// (sound), and a blanket one that catches Exception (masks a bug).
// This is candidate A's `try?` reduced to what the language already has.
module P12;

public class Attempt
  // Names the expected failure: only FormatException/OverflowException become null.
  public static method ParseInt(f: () -> int): int?
    try
      return f();
    catch
      FormatException | OverflowException => return null;
    end try;
  end ParseInt;

  // Blanket form: every catchable exception becomes null.
  public static method Any(f: () -> int): int?
    try
      return f();
    catch
      Exception => return null;
    end try;
  end Any;
end Attempt;

public class Program
  public static method Lookup(xs: Array<int>, text: string): int
    var i := int.Parse(text);
    return xs[i + 10];                 // a bug: wrong index arithmetic
  end Lookup;

  public static method Main()
    var xs := new Array<int>(3, 7);

    var a := Attempt.ParseInt(lambda() => int.Parse("12x"));
    Console.WriteLine($"named failure, bad input : {a = null}");

    var b := Attempt.Any(lambda() => Program.Lookup(xs, "1"));
    Console.WriteLine($"blanket form hides a bug : {b = null}");

    try
      var c := Attempt.ParseInt(lambda() => Program.Lookup(xs, "1"));
      Console.WriteLine(c = null);
    catch
      IndexOutOfRangeException => Console.WriteLine("named form lets the bug through: IndexOutOfRangeException"),
    end try;
    xs.Dispose();
  end Main;
end Program;
