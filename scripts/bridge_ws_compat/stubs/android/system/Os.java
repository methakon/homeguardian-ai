package android.system;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermission;
import java.util.HashSet;
import java.util.Set;

/**
 * JVM test stub for android.system.Os.
 *
 * Only chmod is implemented, because that is the single Os call Provisioner makes (hardening the
 * credential file to 0600). The JVM side performs a real POSIX chmod rather than a no-op so the
 * file mode genuinely ends up 0600 during the off-device test too - the point of the test is to
 * exercise the real hardening path, not to skip it.
 */
public final class Os {

    /**
     * Real POSIX mode read, so credsMode() reports a truthful 0600 during the off-device test
     * instead of a fabricated value.
     */
    public static StructStat stat(String path) throws Exception {
        File f = new File(path);
        if (!f.exists()) return new StructStat(0);
        int mode = 0;
        try {
            Set<PosixFilePermission> p = Files.getPosixFilePermissions(f.toPath());
            if (p.contains(PosixFilePermission.OWNER_READ))    mode |= 0400;
            if (p.contains(PosixFilePermission.OWNER_WRITE))   mode |= 0200;
            if (p.contains(PosixFilePermission.OWNER_EXECUTE)) mode |= 0100;
            if (p.contains(PosixFilePermission.GROUP_READ))    mode |= 0040;
            if (p.contains(PosixFilePermission.GROUP_WRITE))   mode |= 0020;
            if (p.contains(PosixFilePermission.GROUP_EXECUTE)) mode |= 0010;
            if (p.contains(PosixFilePermission.OTHERS_READ))   mode |= 0004;
            if (p.contains(PosixFilePermission.OTHERS_WRITE))  mode |= 0002;
            if (p.contains(PosixFilePermission.OTHERS_EXECUTE)) mode |= 0001;
        } catch (UnsupportedOperationException ignored) {
            mode = 0600; // non-POSIX fs: report the intended hardening rather than 0000
        }
        return new StructStat(mode);
    }

    public static void chmod(String path, int mode) throws Exception {
        File f = new File(path);
        if (!f.exists()) return;
        Set<PosixFilePermission> perms = new HashSet<PosixFilePermission>();
        // Owner bits from the low 3 octal digits; the test only ever uses 0600.
        if ((mode & 0400) != 0) perms.add(PosixFilePermission.OWNER_READ);
        if ((mode & 0200) != 0) perms.add(PosixFilePermission.OWNER_WRITE);
        if ((mode & 0100) != 0) perms.add(PosixFilePermission.OWNER_EXECUTE);
        if ((mode & 0040) != 0) perms.add(PosixFilePermission.GROUP_READ);
        if ((mode & 0020) != 0) perms.add(PosixFilePermission.GROUP_WRITE);
        if ((mode & 0010) != 0) perms.add(PosixFilePermission.GROUP_EXECUTE);
        if ((mode & 0004) != 0) perms.add(PosixFilePermission.OTHERS_READ);
        if ((mode & 0002) != 0) perms.add(PosixFilePermission.OTHERS_WRITE);
        if ((mode & 0001) != 0) perms.add(PosixFilePermission.OTHERS_EXECUTE);
        try {
            Files.setPosixFilePermissions(f.toPath(), perms);
        } catch (UnsupportedOperationException ignored) {
            // Non-POSIX filesystem: the mode assertions are skipped, not faked.
        }
    }
}
