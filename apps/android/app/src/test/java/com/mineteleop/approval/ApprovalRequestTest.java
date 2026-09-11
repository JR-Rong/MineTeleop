package com.mineteleop.approval;
import org.junit.Test;
import static org.junit.Assert.*;

public class ApprovalRequestTest {
    @Test public void pendingExpiresAtMonotonicDeadline() {
        ApprovalRequest request = new ApprovalRequest("r", "v", "d", "pending", 1500, 100);
        assertEquals(2, request.remainingSeconds(100)); assertTrue(request.actionable(1599));
        assertFalse(request.actionable(1600)); assertEquals(0, request.remainingSeconds(9999));
    }
    @Test public void terminalAndUnknownStatesCannotApprove() {
        for (String state : new String[]{"approved", "rejected", "expired", "cancelled", "consumed", "unknown"})
            assertFalse(new ApprovalRequest("r", "v", "d", state, 1000, 0).actionable(0));
    }
    @Test public void invalidOrUnboundedTtlCannotExtendApprovalForever() {
        assertFalse(new ApprovalRequest("r", "v", "d", "pending", -1, 0).actionable(0));
        assertFalse(new ApprovalRequest("r", "v", "d", "pending", Long.MAX_VALUE, 0).actionable(600000));
    }
}
