package android.content;

/** Minimal android.content.Context stand-in carrying the connectivity service. */
public class Context {
    public static final String CONNECTIVITY_SERVICE = "connectivity";

    private Object connectivityService;

    public Object getSystemService(String name) {
        if (CONNECTIVITY_SERVICE.equals(name)) return connectivityService;
        return null;
    }

    public void setConnectivityService(Object service) { this.connectivityService = service; }
}
