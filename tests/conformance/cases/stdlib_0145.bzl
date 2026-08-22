/// Compiler-intrinsic; aliased by the keyword `bool`.
public record Boolean: IEquatable<bool>
  public static pure method Parse(s: string): bool;            // "true"/"false", case-insensitive
  public static method TryParse(s: string, out result: bool): bool;
  public static pure method ParseOrNull(s: string): bool?;     // v0.231 (§1 *OrNull tier): null exactly
                                                               //   where Parse(s) raises; otherwise the
                                                               //   parsed value

  public pure method ToString(): string;                       // "True" or "False"
end Boolean;
