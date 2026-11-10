// Probe p01: one operation, three call-site stances, three stdlib members.
// stdlib §1 (Try convention, *OrNull tier), §4.8, §9.1.
module P01;

public class Program
  public static method Main()
    var ports := new Dictionary<string, int>();
    ports.Add("http", 80);

    // Tier 1, throwing form: absence is a bug at this call site.
    try
      var a := ports["smtp"];
      Console.WriteLine(a);
    catch
      var ex: KeyNotFoundException => Console.WriteLine("indexer threw KeyNotFoundException"),
    end try;

    // Tier 2, Try shape (a): bool + nullable out, narrowed in the true region.
    if ports.TryGetValue("smtp", out var b) then
      Console.WriteLine(b);
    else
      Console.WriteLine("TryGetValue false");
    end if;

    // Tier 3, *OrNull: nullable return, composes with ??.
    var c := ports.GetOrNull("smtp") ?? -1;
    Console.WriteLine($"GetOrNull ?? -1 = {c}");

    // The assert stance rebuilt from the value tier (what the throwing tier is):
    try
      var d := ports.GetOrNull("smtp") ?? throw new InvalidOperationException("no smtp port configured");
      Console.WriteLine(d);
    catch
      var ex: InvalidOperationException => Console.WriteLine($"?? throw: {ex.Message}"),
    end try;
    try
      var e := ports.GetOrNull("smtp")!;
      Console.WriteLine(e);
    catch
      var ex: NullReferenceException => Console.WriteLine($"! on null: NullReferenceException '{ex.Message}'"),
    end try;

    // Parse: throwing form and Try shape (c).
    try
      var n := int.Parse("12x");
      Console.WriteLine(n);
    catch
      var ex: FormatException => Console.WriteLine($"Parse threw FormatException: {ex.Message}"),
    end try;
    if int.TryParse("12x", out var m) then
      Console.WriteLine(m);
    else
      Console.WriteLine($"TryParse false, out = {m}");
    end if;
    Console.WriteLine(bool.ParseOrNull("yes") = null);

    // Overflow is an input failure at Parse and a bug in arithmetic: one type, two bands.
    try
      var big := int.Parse("99999999999");
      Console.WriteLine(big);
    catch
      var ex: OverflowException => Console.WriteLine("Parse overflow: OverflowException"),
    end try;
    Console.WriteLine(int.TryParse("99999999999", out var big2));

    // A caller bug is not converted by the value tier (stdlib §9.1: invalid radix).
    try
      var ok := int.TryParse("12", 7, out var r);
      Console.WriteLine(ok);
    catch
      var ex: ArgumentException => Console.WriteLine("TryParse(radix 7): ArgumentException"),
    end try;
  end Main;
end Program;
