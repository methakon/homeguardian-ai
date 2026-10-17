package android.system;

import java.io.File;

public final class Os {
    public static class StructStat {
        public int st_mode;
        public StructStat(int mode) {
            this.st_mode = mode;
        }
    }

    private Os() {}

    public static void chmod(String path, int mode) throws Exception {
        File f = new File(path);
        if (f.exists()) {
            boolean r = (mode & 0400) != 0;
            boolean w = (mode & 0200) != 0;
            boolean x = (mode & 0100) != 0;
            f.setReadable(r, true);
            f.setWritable(w, true);
            f.setExecutable(x, true);
        }
    }

    public static StructStat stat(String path) {
        return new StructStat(0600);
    }
}
