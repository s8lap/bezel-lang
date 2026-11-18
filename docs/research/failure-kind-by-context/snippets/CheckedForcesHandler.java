// The caller for whom a missing file is fatal writes no handler, as the Go, Rust and Python
// callers may. javac rejects this file: the checked type decides for every caller.
import java.nio.file.Files;
import java.nio.file.Path;

public class CheckedForcesHandler {
    static byte[] openRequiredFile(String path) {
        return Files.readAllBytes(Path.of(path));
    }
}
