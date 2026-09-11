package com.mineteleop.approval;

import java.net.URI;
import java.util.Locale;

final class ApiOrigin {
    static final String DEFAULT = "https://60-205-213-254.nip.io:6000";
    static String normalize(String input, boolean development) {
        try {
            URI uri = new URI(input.trim());
            String host = uri.getHost();
            String scheme = uri.getScheme() == null ? "" : uri.getScheme().toLowerCase(Locale.ROOT);
            boolean local = "localhost".equalsIgnoreCase(host) || "127.0.0.1".equals(host) || "10.0.2.2".equals(host);
            if (host == null || uri.getUserInfo() != null || uri.getQuery() != null || uri.getFragment() != null ||
                !("".equals(uri.getRawPath()) || "/".equals(uri.getRawPath())) ||
                uri.getPort() == 0 || uri.getPort() > 65535 ||
                !("https".equals(scheme) || (development && local && "http".equals(scheme)))) {
                throw new IllegalArgumentException();
            }
            return scheme + "://" + uri.getRawAuthority();
        } catch (Exception error) {
            throw new IllegalArgumentException("请输入 HTTPS 云端地址，例如 https://teleop.example.com:6000");
        }
    }
    private ApiOrigin() {}
}
