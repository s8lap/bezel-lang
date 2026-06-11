public static class FileOps
  public static method OpenChecked(path: CString): int
    var fd := open(path, O_RDONLY);
    if fd < 0 then
      throw new IOException($"open failed: errno={errno()}");
    end;
    return fd;
  end OpenChecked;
end FileOps;
