public class RawBytes
  // No public constructor; obtained via factory methods.

  public static method From(buffer: Array<byte>): owned RawBytes;
  public static method FromForeign(p: IntPtr, length: int,
      freeOnDispose: bool := false): owned RawBytes;    // v0.186 — see prose

  public readonly property Length: int;
  public readonly property Pointer: IntPtr;

  public method CopyTo(destination: Array<byte>, destOffset: int);
      // copies Length bytes into destination[destOffset .. destOffset+Length);
      //   a window outside destination's Count raises IndexOutOfRangeException
      //   (the §1/§7.1 window rule); on a FromForeign view the read is over
      //   foreign memory — spec §32.10 trust applies (v0.195)

  destructor;       // releases the underlying buffer if owned by this instance
end RawBytes;
