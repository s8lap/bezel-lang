try
  BeginBatch();
  DoWork();                        // may throw
finally
  EndBatch();                      // must run even on failure; not a disposal
end;
