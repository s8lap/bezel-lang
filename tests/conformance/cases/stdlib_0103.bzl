/// Compiler-intrinsic; aliased by the keyword `bool`.
public record Boolean: IEquatable<bool>
  public static method Parse(s: string): bool;            // "true"/"false", case-insensitive
  public static method TryParse(s: string, out result: bool): bool;

  public method ToString(): string;                       // "True" or "False"
end Boolean;
