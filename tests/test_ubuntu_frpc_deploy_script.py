import os
import subprocess
import unittest
from pathlib import Path


SCRIPT = Path("scripts/setup_ubuntu_frpc.sh")


class UbuntuFrpcDeployScriptTests(unittest.TestCase):
    def test_script_exists_is_executable_and_has_valid_shell_syntax(self):
        self.assertTrue(SCRIPT.is_file(), f"{SCRIPT} should exist")
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} should be executable")

        result = subprocess.run(["bash", "-n", str(SCRIPT)], text=True, capture_output=True)

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dry_run_renders_default_ubuntu_ssh_proxy_on_remote_port_6000(self):
        result = subprocess.run(
            ["bash", str(SCRIPT), "--dry-run", "--token", "unit-test-token"],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn('serverAddr = "60.205.213.254"', result.stdout)
        self.assertIn("serverPort = 7000", result.stdout)
        self.assertIn('auth.method = "token"', result.stdout)
        self.assertIn('auth.token = "unit-test-token"', result.stdout)
        self.assertIn('auth.additionalScopes = ["HeartBeats", "NewWorkConns"]', result.stdout)
        self.assertIn("transport.tls.enable = true", result.stdout)
        self.assertIn("transport.tcpMux = false", result.stdout)
        self.assertIn('name = "ubuntu-ssh-6000"', result.stdout)
        self.assertIn('localIP = "127.0.0.1"', result.stdout)
        self.assertIn("localPort = 22", result.stdout)
        self.assertIn("remotePort = 6000", result.stdout)
        self.assertIn("ExecStart=/usr/local/bin/frpc -c /etc/frp/frpc.toml", result.stdout)
        self.assertIn("http://60.205.213.254/frp/releases/download/v0.69.1/frp_0.69.1_linux_", result.stdout)
        self.assertNotIn("<replace-with", result.stdout)

    def test_dry_run_allows_custom_ports_and_proxy_name(self):
        result = subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--dry-run",
                "--token",
                "unit-test-token",
                "--proxy-name",
                "ubuntu-alt-ssh",
                "--local-ip",
                "0.0.0.0",
                "--local-port",
                "2222",
                "--remote-port",
                "6001",
            ],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn('name = "ubuntu-alt-ssh"', result.stdout)
        self.assertIn('localIP = "0.0.0.0"', result.stdout)
        self.assertIn("localPort = 2222", result.stdout)
        self.assertIn("remotePort = 6001", result.stdout)

    def test_dry_run_accepts_skip_package_install_for_hosts_with_broken_apt(self):
        result = subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--dry-run",
                "--skip-package-install",
                "--token",
                "unit-test-token",
            ],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("remotePort = 6000", result.stdout)

    def test_help_documents_skip_package_install_recovery_option(self):
        result = subprocess.run(
            ["bash", str(SCRIPT), "--help"],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("--skip-package-install", result.stdout)
        self.assertIn("--download-base-url", result.stdout)
        self.assertIn("broken apt", result.stdout)

    def test_dry_run_allows_custom_download_base_url(self):
        result = subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--dry-run",
                "--token",
                "unit-test-token",
                "--download-base-url",
                "http://mirror.example/frp/releases/download/",
            ],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("http://mirror.example/frp/releases/download/v0.69.1/frp_0.69.1_linux_", result.stdout)

    def test_download_logic_retries_curl_and_falls_back_to_wget(self):
        content = SCRIPT.read_text(encoding="utf-8")

        self.assertIn("curl -fsSL --retry", content)
        self.assertIn("--retry-all-errors", content)
        self.assertIn("warning: curl download failed; trying wget", content)
        self.assertIn("wget -q --tries=", content)


if __name__ == "__main__":
    unittest.main()
