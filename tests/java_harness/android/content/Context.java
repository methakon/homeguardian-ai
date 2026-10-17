package android.content;

import java.io.File;

public class Context {
    private final File filesDir;

    public Context(File filesDir) {
        this.filesDir = filesDir;
        if (!this.filesDir.exists()) {
            this.filesDir.mkdirs();
        }
    }

    public File getFilesDir() {
        return filesDir;
    }
}
