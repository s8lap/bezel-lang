public async method WithCleanupAsync(): Task
  try
    await DoWorkAsync();
  catch
    var ex: Exception => do
      await LogErrorAsync(ex);
      throw;
    end
  finally
    await ReleaseResourcesAsync();
  end;
end WithCleanupAsync;
