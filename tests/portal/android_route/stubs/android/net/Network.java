package android.net;

import java.io.IOException;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * Minimal android.net.Network stand-in that records which network a production binder chose.
 *
 * bindFailure lets a test reproduce the API 30 SocketException/EPERM case (app excluded from the
 * VPN) or a lost network, and bindAttempts proves the binder did not retry another network.
 */
public class Network {
    private final String name;
    public final List<Socket> boundSockets = new ArrayList<Socket>();
    public int bindAttempts = 0;
    public Throwable bindFailure = null;

    public Network(String name) { this.name = name; }

    public void bindSocket(Socket s) throws IOException {
        bindAttempts++;
        if (bindFailure instanceof IOException) throw (IOException) bindFailure;
        if (bindFailure instanceof RuntimeException) throw (RuntimeException) bindFailure;
        boundSockets.add(s);
    }

    public String toString() { return name; }
}
