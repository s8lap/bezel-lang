public class WString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned WString;

  public readonly property Length: int;       // length in UTF-16 code units, excluding the null terminator
  public readonly property Pointer: IntPtr;   // v0.186; address of the buffer — see prose

  destructor;       // releases the underlying buffer
end WString;
