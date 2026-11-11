// Java: the "is this expected?" decision is in the exception's TYPE. IOException is checked,
// so every caller must handle or declare it, including the caller for whom it is fatal;
// NumberFormatException is unchecked, so nothing asks the caller who parses user input.
import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.Map;

public class Demo {
    static void call() throws IOException {
        try (Socket s = new Socket()) {
            s.connect(new InetSocketAddress("127.0.0.1", 1), 300);
        }
    }

    // 1a. A file the user chose.
    static void openUserFile(String path) {
        try {
            byte[] d = Files.readAllBytes(Path.of(path));
            System.out.println("user-file: " + d.length + " bytes");
        } catch (NoSuchFileException e) {
            System.out.println("user-file: no such file, pick another");
        } catch (IOException e) {
            System.out.println("user-file: cannot read: " + e);
        }
    }

    // 1b. The database the service cannot run without. The type still says "recoverable",
    // so the caller writes a handler whose only job is to say otherwise. The wrapper class
    // is the JDK's own (java.io.UncheckedIOException, added in Java 8).
    static byte[] openRequiredFile(String path) {
        try {
            return Files.readAllBytes(Path.of(path));
        } catch (IOException e) {
            throw new UncheckedIOException("required-file: cannot start", e);
        }
    }

    // 2a. Optional telemetry.   2b. The primary upstream: declared and passed up.
    static void sendTelemetry() {
        try {
            call();
        } catch (IOException e) {
            System.out.println("telemetry: dropped one sample");
        }
    }

    static void handleRequest() throws IOException {
        call();
    }

    // 3a. A number the user typed: an unchecked exception, so the handler is optional
    // exactly where it is needed.   3b. A constant: no handler, and none is asked for.
    static void parseUser(String s) {
        try {
            System.out.println("user-number: " + Integer.parseInt(s));
        } catch (NumberFormatException e) {
            System.out.println("user-number: not a number, ask again");
        }
    }

    static int parseConstant(String s) {
        return Integer.parseInt(s);
    }

    // 4a. A user's key: `get` returns null.   4b. The program's own key.
    static final Map<String, Integer> LIMITS = Map.of("retries", 3);

    static void lookupUser(String k) {
        Integer v = LIMITS.get(k);
        System.out.println(v == null ? "user-key: unknown setting" : "user-key: " + v);
    }

    static int lookupOwn(String k) {
        Integer v = LIMITS.get(k);
        if (v == null) throw new IllegalStateException("own-key: missing built-in setting " + k);
        return v;
    }

    public static void main(String[] a) {
        switch (a[0]) {
            case "user-file" -> openUserFile("/nonexistent/notes.txt");
            case "required-file" -> openRequiredFile("/nonexistent/service.db");
            case "telemetry" -> sendTelemetry();
            case "primary-api" -> {
                try {
                    handleRequest();
                } catch (IOException e) { // the request boundary
                    System.out.println("primary-api: 502 Bad Gateway");
                }
            }
            case "user-number" -> parseUser("12a");
            case "constant-number" -> System.out.println("constant-number: " + parseConstant("8080"));
            case "constant-number-wrong" -> System.out.println(parseConstant("80a0"));
            case "user-key" -> lookupUser("colour");
            case "own-key" -> System.out.println("own-key: " + lookupOwn("retries"));
            case "own-key-wrong" -> System.out.println(lookupOwn("retires"));
            default -> { }
        }
    }
}
