// curl_easy_setopt(CURL*, CURLoption, ...) — one trailing arg, type keyed by option:
extern "libcurl"
  public method SetOptLong(h: IntPtr, opt: int, value: long): int
      := symbol "curl_easy_setopt" variadic(2);
  public method SetOptStr(h: IntPtr, opt: int, value: CString): int
      := symbol "curl_easy_setopt" variadic(2);
  public method SetOptPtr(h: IntPtr, opt: int, value: IntPtr): int
      := symbol "curl_easy_setopt" variadic(2);
  public method SetOptWriteFn(h: IntPtr, opt: int,
      cb: (Ptr<byte>, UIntPtr, UIntPtr, IntPtr) -> UIntPtr): int
      := symbol "curl_easy_setopt" variadic(2);   // a static callback plugs in here (§32.8)
end;

// open(2) with O_CREAT — open is ITSELF variadic (int open(const char*, int, ...)),
// so BOTH arities go through the clause; a bare 2-ary `method open(...)` declaration
// would be exactly the %al trap above:
extern "libc"
  public method Open2(path: CString, flags: int): int
      := symbol "open" variadic(2);          // N = declared arity: a variadic call with
                                             //   ZERO variadic args — the SOUND spelling
                                             //   of the fixed-arity workaround
  public method OpenCreate(path: CString, flags: int, mode: uint): int
      := symbol "open" variadic(2);
end;
