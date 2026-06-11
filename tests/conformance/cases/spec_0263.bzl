try
  using var fs := new FileStream(path);
  ProcessFile(fs);
finally
  Log("file processing finished");
end;
