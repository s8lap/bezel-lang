extern "libc"
  public method getenv(name: CString): IntPtr;     // returns a borrowed pointer
end;

public static class Env
  public static method GetVariable(name: string): string?
    using var key := CString.From(name);
    var resultPtr := getenv(key);
    if resultPtr = IntPtr.Zero then
      return null;
    end;
    return string.FromCStringPointer(resultPtr);   // copy; getenv buffer untouched
  end GetVariable;
end Env;
