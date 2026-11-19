"""Shipped YAML configuration files must be usable out of the box.

The packages install the YAML equivalents of the classic configuration
files next to them (issue #12592). These tests boot Fluent Bit against
the actual files from the repository 'conf/' directory to make sure a
fresh installation can start from the YAML format without any manual
conversion.
"""

import time
from pathlib import Path

import pytest

from utils.fluent_bit_manager import FluentBitManager
from utils.test_service import FluentBitTestService

REPO_ROOT = Path(__file__).resolve().parents[5]
SHIPPED_CONF_DIR = REPO_ROOT / "conf"


def wait_for_log(log_file, predicate, *, timeout=15, description="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            text = Path(log_file).read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        if predicate(text):
            return
        time.sleep(0.5)
    pytest.fail(f"timed out waiting for {description}")


def test_shipped_main_yaml_boots(tmp_path):
    work_dir = tmp_path / "conf"
    work_dir.mkdir()
    # The shipped file references 'parsers.yaml' and 'plugins.yaml'
    # relative to the configuration directory, so stage the set the way
    # an installation does.
    for name in ("fluent-bit.yaml", "parsers.yaml", "plugins.yaml"):
        source = SHIPPED_CONF_DIR / name
        assert source.is_file(), f"missing shipped file: {source}"
        (work_dir / name).write_text(source.read_text(encoding="utf-8"),
                                     encoding="utf-8")

    manager = FluentBitManager(str(work_dir / "fluent-bit.yaml"))
    try:
        # The shipped file keeps the metrics HTTP server disabled, so start
        # without waiting for the readiness endpoint.
        manager.start(wait_for_ready=False)
        wait_for_log(
            manager.log_file,
            lambda text: "[input:cpu:cpu.0] initializing" in text,
            description="cpu input initialized from the shipped YAML",
        )
        wait_for_log(
            manager.log_file,
            lambda text: "cpu.local" in text,
            description="records flowing through the default pipeline",
        )
        text = Path(manager.log_file).read_text(encoding="utf-8",
                                                errors="replace")
        assert "error" not in text.lower()
    finally:
        manager.stop()


def test_shipped_parsers_yaml_registers_parsers(tmp_path):
    work_dir = tmp_path / "conf"
    work_dir.mkdir()
    (work_dir / "parsers.yaml").write_text(
        (SHIPPED_CONF_DIR / "parsers.yaml").read_text(encoding="utf-8"),
        encoding="utf-8")

    config = work_dir / "fluent-bit.yaml"
    config.write_text(
        "service:\n"
        "  flush: 0.2\n"
        "  grace: 1\n"
        "  http_server: on\n"
        "  http_port: ${FLUENT_BIT_HTTP_MONITORING_PORT}\n"
        "  parsers_file: parsers.yaml\n"
        "pipeline:\n"
        "  inputs:\n"
        "    - name: tail\n"
        "      path: " + str(work_dir / "test.log") + "\n"
        "      parser: apache\n"
        "      read_from_head: true\n"
        "      refresh_interval: 1\n"
        "  outputs:\n"
        "    - name: stdout\n"
        "      match: '*'\n"
        "      format: json_lines\n",
        encoding="utf-8")

    (work_dir / "test.log").write_text(
        '127.0.0.1 - - [19/Nov/2026:10:38:00 +0000] '
        '"GET /index.html HTTP/1.1" 200 1234\n',
        encoding="utf-8")

    service = FluentBitTestService(str(config))
    try:
        service.start()
        service.wait_for_condition(
            lambda: '"code":"200"' in Path(service.flb.log_file).read_text(
                encoding="utf-8"),
            timeout=15,
            description="apache parser applied from the shipped parsers.yaml",
        )
    finally:
        service.stop()
