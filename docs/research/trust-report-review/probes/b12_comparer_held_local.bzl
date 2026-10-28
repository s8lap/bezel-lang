module Conf;
// Comparer-held clause with a scope-bound LOCAL comparer and a field-held dictionary: the retained borrow dangles after the constructor. Spec: the retention is read off the signature ("take a plain parameter and retain it") and is recorded trust. EXPECT: report (or a rejection if the checker sees the store); measure.
public class CaseFold: IEqualityComparer<string>
  public method Equals(a: string, b: string): bool => a = b;
  public method GetHashCode(value: string): int => value.Length;
end CaseFold;
public class Index
  private owned _map: Dictionary<string, int>;
  public constructor()
    using var cmp := new CaseFold();
    _map := new Dictionary<string, int>(cmp);
  end constructor;
  public method Count(): int => _map.Count;
end Index;
