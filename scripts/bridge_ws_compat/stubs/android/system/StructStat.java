package android.system;

/**
 * JVM test stub for android.system.StructStat. Only st_mode is populated, because that is the only
 * field Provisioner's credsMode() reads to report the credential file's mode.
 */
public final class StructStat {
    public int st_mode;
    public StructStat(int st_mode) { this.st_mode = st_mode; }
}
