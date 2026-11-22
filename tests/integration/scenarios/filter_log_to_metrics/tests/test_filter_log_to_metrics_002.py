import os

from utils.test_service import FluentBitTestService


GAUGE_SERIES = "host_gauge_used_percent{block_device=\"vda\"}"
GAUGE_SERIES_TEMPLATE = "host_gauge_used_percent{block_device=\"%s\"}"
DEVICES = ("vda", "vdb")


class Service:
    def __init__(self, config_file):
        self.config_file = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "../config", config_file)
        )
        self.service = FluentBitTestService(self.config_file)

    def start(self):
        self.service.start()

    def stop(self):
        self.service.stop()

    def read_log(self):
        with open(self.service.flb.log_file, encoding="utf-8",
                  errors="replace") as log_file:
            return log_file.read()


def test_log_to_metrics_immediate_add_label_no_duplicate_series():
    service = Service("immediate_add_label_stdout.yaml")

    try:
        service.start()

        # Wait until both device series have been emitted at least once.
        service.service.wait_for_condition(
            lambda: (
                text
                if all(
                    (GAUGE_SERIES_TEMPLATE % device) in
                    (text := service.read_log())
                    for device in DEVICES
                )
                else None
            ),
            timeout=15,
            interval=1,
            description="both device gauges emitted to stdout",
        )

        # Issue #12107: the metrics context is a cumulative snapshot, so it
        # must be appended at most once per flush window. Appending it on
        # every filter invocation re-emits series already sent in the same
        # window, and stdout prints one line per appended series.
        text = service.read_log()
        for device in DEVICES:
            series = GAUGE_SERIES_TEMPLATE % device
            count = text.count(series)
            assert count == 1, (
                f"expected exactly one '{series}' sample, found {count}:\n{text}"
            )
    finally:
        service.stop()
