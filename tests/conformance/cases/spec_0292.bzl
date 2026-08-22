public static class FileOps
  public static method OpenForReading(path: string): int
    using var cpath := CString.From(path);   // owned CString; disposed by using var
    return open(cpath, O_RDONLY);
  end OpenForReading;
end FileOps;
