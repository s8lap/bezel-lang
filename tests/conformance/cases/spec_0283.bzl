extern "libc"
  public method puts(s: CString): int;
  public method strlen(s: CString): UIntPtr;
  public method strdup(s: CString): owned CString?;   // caller-owned on non-NULL return (§32.6)
end;
