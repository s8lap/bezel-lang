try
  using var fs := new FileStream(path, FileMode.Open);
  ProcessFile(fs);
finally
  Log("file processing finished");
end;
