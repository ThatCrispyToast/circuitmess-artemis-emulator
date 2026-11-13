import datetime as dt
import os
import signal

import pytest

from artemis_emu import Watch

STOCK = os.environ.get("ARTEMIS_STOCK_FIRMWARE")
MICROPYTHON = os.environ.get("ARTEMIS_MICROPYTHON_FIRMWARE")

# The firmware plays its intro for about nine seconds before the lock screen.
BOOT_TIME = 14


@pytest.fixture(autouse=True)
def time_limit():
    """Fails a test whose firmware hangs instead of stalling the whole run."""

    def expired(signum, frame):
        raise TimeoutError("test ran for more than 120 s")

    signal.signal(signal.SIGALRM, expired)
    signal.alarm(120)
    yield
    signal.alarm(0)


@pytest.fixture
def stock():
    if not STOCK:
        pytest.skip("ARTEMIS_STOCK_FIRMWARE isn't set")
    with Watch(STOCK, time=dt.datetime(2026, 7, 20, 20, 17)) as watch:
        yield watch


@pytest.fixture
def booted(stock):
    """Stock firmware that has finished booting and is showing the lock screen."""
    stock.wait_for_output("Hello", timeout=10)
    stock.wait_until(lambda: stock.state.frame_seq > 0, timeout=10)
    import time

    time.sleep(BOOT_TIME)
    return stock


def pytest_collection_modifyitems(items):
    if not MICROPYTHON:
        skip = pytest.mark.skip(reason="ARTEMIS_MICROPYTHON_FIRMWARE isn't set")
        for item in items:
            if item.module.__name__ == "test_micropython":
                item.add_marker(skip)


@pytest.fixture
def micropython():
    from artemis_emu import PythonSession

    with Watch(MICROPYTHON, time=dt.datetime(2026, 7, 20, 20, 17)) as watch:
        yield PythonSession(watch)
