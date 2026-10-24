package org.homeguardian.edgeservice;

import android.content.Context;
import android.system.Os;
import android.system.OsConstants;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.util.Locale;

/** Local UI-only camera provisioning. No intent extras, exported endpoints or logs.
 * Config and registry share ONE atomic 0600 document, so a crash cannot combine
 * a newly selected host with stale consent. Existing MCP/bridge files are untouched.
 */
final class CameraProvisioner {
    private static File file(Context c) { return new File(c.getFilesDir(), "camera_local.json"); }
    private CameraProvisioner() {}

    static synchronized JSONObject read(Context c) throws Exception {
        File f = file(c);
        if (!f.exists()) return new JSONObject();
        if (f.length() > 16384) throw new Exception("camera configuration refused");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[1024]; int n;
            while ((n = in.read(b)) != -1) {
                if (bytes.size() + n > 16384) throw new Exception("camera configuration refused");
                bytes.write(b, 0, n);
            }
        }
        return new JSONObject(new String(bytes.toByteArray(), "UTF-8"));
    }

    static synchronized void save(Context c, String host, String user, String password,
                                  String mac, boolean consent, boolean emptyApproved,
                                  boolean bindingApproved) throws Exception {
        // Validation never resolves DNS or contacts a camera.
        if (!consent || !bindingApproved) throw new Exception("explicit consent and host/MAC approval required");
        if (!privateIpv4(host)) throw new Exception("use a private IPv4 literal, without port or path");
        mac = mac.toLowerCase(Locale.ROOT);
        if (!mac.matches("[0-9a-f]{2}(:[0-9a-f]{2}){5}") ||
                mac.equals("00:00:00:00:00:00") || mac.equals("ff:ff:ff:ff:ff:ff") ||
                (Integer.parseInt(mac.substring(0,2),16) & 1) != 0)
            throw new Exception("enter the camera's explicit unicast MAC");
        if (user.length() == 0 || user.length() > 128 || user.indexOf(':') >= 0 ||
                password.length() > 512 || (password.length() == 0 && !emptyApproved))
            throw new Exception("credentials invalid; empty password requires explicit approval");
        JSONObject root = new JSONObject();
        root.put("host", host); root.put("user", user); root.put("password", password); root.put("mac", mac);
        root.put("consent", true); root.put("capture", true);
        root.put("approved_host_mac", true); root.put("empty_password_approved", emptyApproved);
        JSONObject d = new JSONObject();
        d.put("device_id", "edge-local-camera"); d.put("display_name", "Local camera");
        d.put("device_type", "camera"); d.put("credential_id", mac);
        d.put("paired", true); d.put("revoked", false); d.put("has_camera", true);
        d.put("has_microphone", false); d.put("is_synthetic", false);
        d.put("capabilities", new JSONArray().put("camera"));
        d.put("consent_granted", true); d.put("consent_subject", "local-owner");
        d.put("consent_purpose", "home_security"); d.put("consent_updated_ms", System.currentTimeMillis());
        d.put("capture_enabled", true);
        root.put("devices", new JSONArray().put(d));
        // Stop/join BEFORE replacing source identity or its credentials.
        NativeEngine.reloadCamera(false);
        write(c, root);
        NativeEngine.reloadCamera(true);
    }

    static synchronized void disable(Context c) throws Exception {
        // Revoke on disk first, even if a worker is currently inside bounded HTTP.
        JSONObject root;
        try { root = read(c); } catch (Exception ignored) { root = new JSONObject(); }
        root.put("consent", false); root.put("capture", false);
        JSONArray devices = root.optJSONArray("devices");
        if (devices != null) for (int i=0; i<devices.length(); ++i) {
            JSONObject d=devices.getJSONObject(i);
            d.put("capture_enabled", false); d.put("consent_granted", false);
        }
        try { write(c, root); } finally { NativeEngine.reloadCamera(false); }
    }

    private static boolean privateIpv4(String h) {
        if (!h.matches("[0-9]{1,3}(\\.[0-9]{1,3}){3}")) return false;
        String[] parts=h.split("\\."); int[] v=new int[4];
        for (int i=0; i<4; ++i) {
            if (parts[i].length()>1 && parts[i].charAt(0)=='0') return false;
            v[i]=Integer.parseInt(parts[i]); if (v[i]>255) return false;
        }
        return v[3]!=0 && v[3]!=255 && (v[0]==10 || (v[0]==172 && v[1]>=16 && v[1]<=31) ||
                (v[0]==192 && v[1]==168));
    }

    private static void write(Context c, JSONObject root) throws Exception {
        File target=file(c), temp=new File(c.getFilesDir(), "camera_local.json.tmp");
        FileDescriptor fd=Os.open(temp.getAbsolutePath(), OsConstants.O_WRONLY | OsConstants.O_CREAT |
                OsConstants.O_TRUNC | OsConstants.O_NOFOLLOW, 0600);
        try (FileOutputStream out=new FileOutputStream(fd)) {
            Os.fchmod(fd, 0600);
            out.write(root.toString().getBytes("UTF-8")); out.flush(); out.getFD().sync();
        }
        Os.rename(temp.getAbsolutePath(), target.getAbsolutePath());
    }
}
