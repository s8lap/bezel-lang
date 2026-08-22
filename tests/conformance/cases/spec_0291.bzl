extern "sqlite3"
  public method sqlite3_open(filename: CString, out db: IntPtr): int;
  public method sqlite3_close(db: IntPtr): int;
end;

public static class Sqlite
  public static method Open(path: string): IntPtr
    using var cpath := CString.From(path);
    var rc := sqlite3_open(cpath, out var raw);
    if rc <> 0 then
      if raw <> IntPtr.Zero then                 // per SQLite's contract a handle may exist on failure
        _ := sqlite3_close(raw);
      end;
      throw new IOException($"sqlite3_open failed: rc={rc}");
    end;
    return raw;
  end Open;
end Sqlite;
