public async method DownloadAsync(url: string): Task<string>
  using var client := new HttpClient();
  using var task := client.GetAsync(url);
  return await task;
end DownloadAsync;
