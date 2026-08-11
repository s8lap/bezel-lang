import System.Interop;

// C: int sqlite3_exec(sqlite3*, const char* sql,
//                     int (*cb)(void*, int, char**, char**),
//                     void* user, char** errmsg);
extern "sqlite3"
  public method sqlite3_exec(db: IntPtr, sql: CString,
      cb: (env, int, Ptr<IntPtr>, Ptr<IntPtr>) -> int,   // env: where C passes user-data INTO the callback
      env,                                               // env: which extern argument carries it
      errmsg: Ptr<IntPtr>): int;
end;

// C: void qsort_r(void *base, size_t, size_t,
//                 int (*compar)(const void*, const void*, void*), void* arg);
extern "libc"
  public method qsort_r(basePtr: Ptr<byte>, nmemb: UIntPtr, size: UIntPtr,
      compar: (IntPtr, IntPtr, env) -> int, env): ();
end;
