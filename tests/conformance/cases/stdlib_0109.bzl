public class CString
  // No public constructor; obtained via factory methods.

  public static method From(s: string): owned CString;
  public static method FromBytes(bytes: Array<byte>): owned CString;

  public readonly property Length: int;       // byte length, excluding the null terminator
  public method ToBytes(): owned Array<byte>;

  destructor;       // releases the underlying buffer when this CString
                    //   allocated it (CString.From / FromBytes); for a CString
                    //   wrapping a native-allocated buffer, the disposal
                    //   behavior is governed by the FFI declaration. See prose.
end CString;
