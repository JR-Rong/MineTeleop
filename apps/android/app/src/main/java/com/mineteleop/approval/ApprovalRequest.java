package com.mineteleop.approval;

final class ApprovalRequest {
    final String id, vehicle, driver, state;
    final long deadline;
    ApprovalRequest(String id, String vehicle, String driver, String state, long remainingMs, long now) {
        this.id = id; this.vehicle = vehicle; this.driver = driver; this.state = state;
        deadline = now + Math.max(0, Math.min(600000, remainingMs));
    }
    long remainingSeconds(long now) { return Math.max(0, (deadline - now + 999) / 1000); }
    boolean actionable(long now) { return "pending".equals(state) && now < deadline; }
    String label() {
        switch (state) {
            case "pending": return "等待确认";
            case "approved": return "已同意 · 等待连接";
            case "consumed": return "授权已使用";
            case "rejected": return "已拒绝";
            case "expired": return "已过期";
            case "cancelled": return "已取消";
            default: return "状态未知，请刷新";
        }
    }
}
