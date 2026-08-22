public record Sqlite3Db(Raw: IntPtr);
public record Sqlite3Stmt(Raw: IntPtr);

extern "sqlite3"
  public method sqlite3_open(filename: CString, out db: Sqlite3Db): int;
  public method sqlite3_prepare_v2(db: Sqlite3Db, sql: CString, nByte: int,
      out stmt: Sqlite3Stmt, out tail: IntPtr): int;
  public method sqlite3_step(stmt: Sqlite3Stmt): int;
  public method sqlite3_finalize(stmt: Sqlite3Stmt): int;
  public method sqlite3_close(db: Sqlite3Db): int;
end;

public class Database(handle: Sqlite3Db)
  private _handle: Sqlite3Db := handle;

  public property Raw: Sqlite3Db => _handle;

  destructor
    _ := sqlite3_close(_handle);           // policy lives HERE: this binding chooses to
                                           //   ignore SQLITE_BUSY (leak-on-misuse beats
                                           //   crash); another may verify or log
  end;
end Database;
