module Probe;
// CRITIQUE FINDING 6, the rewrite: the comparer is a `static readonly` singleton, so it is static-storage-sourced and contributes no source. EXPECT: compiles; no report line for the comparer; clean in both modes; prints True. Under 0212 revision 2: admitted, and the dictionary is still sendable. RESULT on r40: not runnable, as c06.
public class IgnoreCase: IEqualityComparer<string>
  public static readonly Instance: IgnoreCase := new IgnoreCase();
  public constructor()
  end constructor;
  public method Equals(a: string, b: string): bool => a.ToUpper() = b.ToUpper();
  public method GetHashCode(value: string): int => value.ToUpper().GetHashCode();
end IgnoreCase;
public class Program
  public static method Main(): int
    using var d := new Dictionary<string, int>(IgnoreCase.Instance);
    d.Add("Ann", 1);
    Console.WriteLine(d.ContainsKey("ANN"));
    return 0;
  end Main;
end Program;
