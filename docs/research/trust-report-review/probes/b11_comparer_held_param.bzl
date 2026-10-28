module Conf;
// Comparer-held clause (v0.327, proposal 0138 §7, #5604): a comparer passed to a hash collection's constructor is retained as a borrow; the constructor call is the report site. EXPECT: report at `new Dictionary<string, int>(cmp)`.
public class CaseFold: IEqualityComparer<string>
  public method Equals(a: string, b: string): bool => a = b;
  public method GetHashCode(value: string): int => value.Length;
end CaseFold;
public class Index
  private owned _map: Dictionary<string, int>;
  public constructor(cmp: IEqualityComparer<string>)
    _map := new Dictionary<string, int>(cmp);
  end constructor;
end Index;
