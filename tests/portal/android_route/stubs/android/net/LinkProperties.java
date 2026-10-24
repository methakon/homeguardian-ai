package android.net;

import java.util.ArrayList;
import java.util.List;

/** Minimal android.net.LinkProperties stand-in: just the route list the policy reads. */
public class LinkProperties {
    private final List<RouteInfo> routes = new ArrayList<RouteInfo>();

    public LinkProperties addRoute(RouteInfo route) {
        routes.add(route);
        return this;
    }

    public List<RouteInfo> getRoutes() { return routes; }
}
