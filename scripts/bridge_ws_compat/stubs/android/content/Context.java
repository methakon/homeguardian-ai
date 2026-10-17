package android.content;

import java.io.File;

/**
 * JVM test stub for android.content.Context.
 *
 * Only getFilesDir() is implemented, because that is the ONLY Context surface the classes under
 * test touch: Provisioner and OutboundMcpBridge read the app-private files dir and nothing else.
 * Nothing else is stubbed deliberately - if the real code needed more of Context, this test would
 * fail to compile rather than silently pass against a fake.
 */
public class Context {
    private final File filesDir;

    public Context(File filesDir) { this.filesDir = filesDir; }

    public File getFilesDir() { return filesDir; }
}
