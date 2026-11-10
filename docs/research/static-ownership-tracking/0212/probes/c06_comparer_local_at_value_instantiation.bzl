module Probe;
// CRITIQUE FINDING 6 (sendability of comparer-retaining collections): a `Dictionary<string, int>` built over a LOCAL comparer, which is disposed while the dictionary is still used. `Dictionary<string, int>` is sendable and stays so under revision 2, so its type cannot announce a carried comparer. EXPECT today: compiles (the comparer-held trust clause); checked = DisposedAccessException at the lookup. Under 0212 revision 2: error at the construction — at an instantiation that is not borrow-bearing a `comparer` constructor takes only a comparer that contributes no source (static storage). c07 is the rewrite. RESULT on r40: not runnable — the `IEqualityComparer` constructors are not implemented (the compiler says so), so no program in the corpora can depend on a retained comparer today.
public class IgnoreCase: IEqualityComparer<string>
  public constructor()
  end constructor;
  public method Equals(a: string, b: string): bool => a.ToUpper() = b.ToUpper();
  public method GetHashCode(value: string): int => value.ToUpper().GetHashCode();
end IgnoreCase;
public class Program
  public static method Main(): int
    var cmp := new IgnoreCase();
    using var d := new Dictionary<string, int>(cmp);
    d.Add("Ann", 1);
    cmp.Dispose();
    Console.WriteLine(d.ContainsKey("ANN"));
    return 0;
  end Main;
end Program;
