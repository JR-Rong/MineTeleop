package com.mineteleop.approval;
import org.junit.Test;
import static org.junit.Assert.*;

public class ApiOriginTest {
    @Test public void acceptsHttpsOriginAndTrimsSlash() {
        assertEquals("https://teleop.example.com:6000", ApiOrigin.normalize(" https://teleop.example.com:6000/ ", false));
    }
    @Test public void cleartextOnlyAllowedForDebugLoopback() {
        assertEquals("http://10.0.2.2:18779", ApiOrigin.normalize("http://10.0.2.2:18779", true));
        for (String origin : new String[]{"http://10.0.2.2:18779", "http://127.0.0.1:18779", "http://localhost"}) rejected(origin, false);
        rejected("http://teleop.example.com", true); rejected("http://192.168.1.2", true);
    }
    @Test public void forbidsCredentialAndPathInjection() {
        for (String origin : new String[]{"https://user:password@teleop.example.com", "https://teleop.example.com/mobile/",
                "https://teleop.example.com?token=secret", "https://teleop.example.com#secret", "https://teleop.example.com:0",
                "https://teleop.example.com:65536", "https://", "file:///etc/passwd"}) rejected(origin, true);
    }
    private void rejected(String origin, boolean debug) {
        try { ApiOrigin.normalize(origin, debug); fail("Accepted invalid origin"); }
        catch (IllegalArgumentException expected) { /* expected */ }
    }
}
