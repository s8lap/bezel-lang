extern "libdl"
  public method dlopen(filename: CString, flags: int): IntPtr;
  public method dlclose(handle: IntPtr): int;
end;

public static class DynamicLoader
  public static method LoadAndClose(name: string)
    using var libname := CString.From(name);
    var handle := dlopen(libname, RTLD_NOW);
    if handle = IntPtr.Zero then
      throw new IOException("Library failed to load");
    end;
    _ := dlclose(handle);
  end LoadAndClose;
end DynamicLoader;
