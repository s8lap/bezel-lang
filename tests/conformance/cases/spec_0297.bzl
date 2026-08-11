import System.Interop;

extern "sqlite3"
  public method sqlite3_column_blob(stmt: IntPtr, col: int): Ptr<byte>;   // borrowed; valid
  public method sqlite3_column_bytes(stmt: IntPtr, col: int): int;        //   until next step
end;

public static class Blobs
  public static method ReadBlob(stmt: IntPtr, col: int): owned Array<byte>
    var n := sqlite3_column_bytes(stmt, col);
    var p := sqlite3_column_blob(stmt, col);
    if n = 0 or p = Ptr<byte>.Zero then          // SQLite returns NULL for the empty blob
      return new Array<byte>(0, 0);              // sized ctor (count, fill), stdlib §4.4
    end;
    return p.CopyToArray(n);                     // ONE explicit copy; result is Bezel-owned
  end ReadBlob;
end Blobs;
