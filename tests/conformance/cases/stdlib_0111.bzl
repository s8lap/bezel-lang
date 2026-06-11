public class RawBytes
  // No public constructor; obtained via factory methods.

  public static method From(buffer: Array<byte>): owned RawBytes;

  public readonly property Length: int;
  public readonly property Pointer: IntPtr;

  public method CopyTo(destination: Array<byte>, destOffset: int);

  destructor;       // releases the underlying buffer if owned by this instance
end RawBytes;
