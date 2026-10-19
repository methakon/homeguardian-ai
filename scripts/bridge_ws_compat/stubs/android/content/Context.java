package android.content;

import java.io.File;

/**
 * JVM test stub for android.content.Context.
 */
public class Context {
    public static final String CONNECTIVITY_SERVICE = "connectivity";

    private final File filesDir;

    public Context() { this.filesDir = null; }
    public Context(File filesDir) { this.filesDir = filesDir; }

    public File getFilesDir() { return filesDir; }
    public Object getSystemService(String name) { return null; }
}
