# Windows WLAN streaming-mode investigation, 2026-11-14

**Status: candidate under test. Windows 60-minute acceptance has not passed.** This is Windows Edge to Orin over a local routed link through Mac Internet Sharing. It is not independent WAN/5G or real-vehicle acceptance.

## Reproduced failure before the candidate

PR26 product source was `2b6f30b31cee2d4818b7b07f3d9f6d2ec927bfdd`. The Windows controller was the `88b1bbc` artifact, SHA-256 `e18ed98e3246aa5c0a3eea68648968c25d52436f3c59ccd9028c675caf8e5bf6`; subsequent Windows controller product source was unchanged. The Orin binary included the authenticated profile-rejection fix, SHA-256 `544699c324f5e6b4c49df5e420d7c94a0d4968c57954592ee62099d80c889620`.

All arms used the real pair `192.168.1.55` / `192.168.2.2`, UDP, host candidates, two physical ISX031 cameras, mock adapter, CAN down, zero field/session speed and motor torque, and a vehicle-confirmed five-degree mock steering limit. WASD eight directions and Space brake used ordinary page input handlers and correlated vehicle results/mock steering feedback. The clock limit remained 25 ms, command gap 200 ms, hard watchdog 800 ms, critical-video limit 500 ms, consecutive-late limit three, and transient hold disabled. VPN was not changed.

| Windows setting | Completed cycles | Sample span through failure | Failure |
| --- | ---: | ---: | --- |
| Original Wi-Fi 6, power Auto | 6 | 147.763 s | Forward-left, degraded and input rearm |
| Wi-Fi 6, idle power temporarily disabled | 26 | 651.032 s | Reverse-right, degraded and input rearm |
| Temporarily Wi-Fi 5, power Auto | 0 | 6.460 s | Right, input cleared by vehicle braking-wait feedback |

These are failed prefixes, not completed 60-minute tests. A separate preparation session was intentionally ended after the operator authorized changing the protocol arm; missing-route and unconfirmed-profile preflight receipts are retained separately.

In the second failure, the sender continued at approximately 51 ms intervals with at most 2 ms wakeup lag. Orin command callbacks followed kernel arrival promptly with zero control-mutex wait, but the packet socket observed a **503.385 ms incoming DTLS gap**, followed by a burst. Its drop counter was zero. This localizes that incident before the application callback, without identifying the exact driver or network component.

The Wi-Fi 5 arm added identical 16-byte UDP echo probes to Mac and Orin. At the control failure, their RTT maxima were **331.462 / 331.977 ms**. Windows send intervals stayed approximately 108–111 ms and send calls below 0.25 ms. Mac observed a **367.741 ms receive gap**, then sequences 1278–1280 together; Orin kernel timestamps showed the matching burst. The vehicle entered braking wait with the last valid command 364 ms old. This reproduces delay on ordinary UDP before the Mac receiving point, independently of WebRTC. The unresolved component is on the common Windows-to-Mac segment: NIC, driver, access point or intervening LAN. Browser-only and Orin-callback-only explanations are insufficient.

The prior Mac browser local-link run passed 1,846.570 seconds. Its complete matched command-result RTT was p50/p95/p99/max **16/28/31/59 ms**. The Windows failed prefixes are insufficient for the requested sustained latency comparison. The network paths and browsers also differ; they cannot establish an OS or native-versus-browser advantage. A latest-accepted-result RTT can remain low while newer commands are missing or expired, so it must not replace failed-command and braking-wait evidence.

Original evidence, including all three failed traces, restore receipts, probes and checksums, is at `/Volumes/SystemDisk/Workspace/mine-teleop-artifacts/pr26-winweb60-local-20261114/`. `network-incident.json` SHA-256: `976b29803253548e68a9270fa92a1931b4e8ea2c6bc80aadae430a83ea42e154`. The power and protocol changes were restored, temporary tasks/routes were removed or disappeared on adapter reset, test credentials were deleted, and CAN remained down. Mac Ethernet sharing was intentionally left restored.

## Candidate borrowed from Moonlight

Moonlight requests Windows WLAN media streaming mode for connected wireless interfaces, and releases that client request by closing its WLAN handle. See [the implementation at `94c65fd`](https://github.com/moonlight-stream/moonlight-common-c/blob/94c65fd364d17b223f00a8763c9a5d94a66cb8dd/src/PlatformSockets.c#L849-L938), [Microsoft's opcode contract](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/ne-wlanapi-wlan_intf_opcode) and [the API](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/nf-wlanapi-wlansetinterface). This is a platform hint intended to reduce wireless streaming jitter; effectiveness depends on the driver and is not guaranteed.

`ScopedWifiStreaming` implements a best-effort, client-owned request without changing persistent adapter settings. The browser controller owns it while its application runs; the native viewer owns it during its connection worker. The optional system DLL is loaded from System32. Missing WLAN services or unsupported drivers report an error and preserve existing control behavior. Startup diagnostics record counts/error codes, not interface identities. No WLAN calls run in the periodic control sender. Closing the owner releases its handle; network reconnection can reset the driver mode and is not claimed to be covered by this startup hint.

On the actual Windows device, a short API check observed mode **0 → 1 → 0**, with both set and close returning success. A temporary API wrapper with the original Wi-Fi 6/power settings then completed 17 directional cycles, but failed the continuous-direct criterion at 428.422 seconds when commands switched to WSS. The sampled vehicle state at failure was still `CONTROL_ACTIVE` with stop sequence zero; this is not evidence of a safety-stop failure or of a completed 15-minute run. That wrapper is a prototype and does not validate the new compiled C++ implementation.

While streaming mode was active, querying the separate background-scan flag still returned enabled. A short independent API check successfully changed that flag **1 → 0 → 1** using its own client handle. The candidate therefore also requests `wlan_intf_opcode_background_scan_enabled = FALSE` on connected WLAN interfaces. This is a separate Windows API request, not a claim that Moonlight makes the same additional call. It does not disable WLAN auto-configuration, change the Wi-Fi protocol, alter VPN, or persist registry settings. It is released by closing the same client handle. Unsupported requests remain best-effort and are visible in startup error/count diagnostics. A combined-mode directional short test and the full compiled-client validation remain pending.

Validation so far: macOS platform-source syntax check passed with C++20, `-Wall -Wextra -Werror`; `git diff --check` passed. Windows compilation, compiled-client release verification and a passing 60-minute control run remain required.
