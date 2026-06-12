public class WString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned WString;

  public readonly property Length: int;       // length in UTF-16 code units, excluding the null terminator

  destructor;       // releases the underlying buffer
end WString;
