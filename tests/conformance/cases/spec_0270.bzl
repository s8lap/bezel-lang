extern "libc"
  public method puts(s: CString): int;
  public method strlen(s: CString): IntPtr;
  public method strdup(s: CString): CString?;
end;
