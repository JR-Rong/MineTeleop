package com.mineteleop.approval;

import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class ApprovalApi {
    static final class Failure extends Exception {
        final int status;
        Failure(int status, String message) { super(message); this.status = status; }
    }
    static JSONObject request(String origin, String path, String token, JSONObject body) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(origin + "/mobile/api/" + path).openConnection();
        connection.setConnectTimeout(8000);
        connection.setReadTimeout(8000);
        connection.setInstanceFollowRedirects(false);
        connection.setUseCaches(false);
        connection.setRequestProperty("Accept", "application/json");
        connection.setRequestProperty("Cache-Control", "no-store");
        if (!token.isEmpty()) connection.setRequestProperty("X-Mine-Teleop-Approver-Token", token);
        try {
            if (body != null) {
                connection.setRequestMethod("POST"); connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
                byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
                connection.setFixedLengthStreamingMode(bytes.length);
                try (var output = connection.getOutputStream()) { output.write(bytes); }
            }
            int status = connection.getResponseCode();
            if (status >= 300 && status < 400) throw new Failure(status, "云端地址发生跳转，请填写正确的 HTTPS 地址");
            InputStream stream = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            if (stream == null) throw new Failure(status, "云端未返回有效响应");
            String payload;
            try (InputStream input = stream; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[4096]; int count;
                while ((count = input.read(buffer)) != -1) {
                    if (output.size() + count > 1024 * 1024) throw new Failure(status, "云端响应过大");
                    output.write(buffer, 0, count);
                }
                payload = output.toString(StandardCharsets.UTF_8.name());
            }
            JSONObject result = new JSONObject(payload);
            if (status < 200 || status >= 300) throw new Failure(status, result.optString("error", "请求失败"));
            return result;
        } finally { connection.disconnect(); }
    }
    private ApprovalApi() {}
}
