public abstract class DataSource
  public abstract method LoadAsync(): Task<string>;
  public abstract method SaveAsync(data: string): Task;
end DataSource;

public class FileSource: DataSource
  public async override method LoadAsync(): Task<string>
    using var fs := new FileStream(_path);
    return await fs.ReadAllAsync();
  end LoadAsync;

  public async override method SaveAsync(data: string): Task
    await File.WriteAllTextAsync(_path, data);
  end SaveAsync;
end FileSource;
