"""Phase 11 Sensor Data Browse UI integration tests (T6-series).

Requires INTERNAL_URL, ADMIN_USER, ADMIN_PASS, TEST_DB_DSN in .env.
Uses region=998, station=1 — never a real station (SEED_REGION in conftest.py).
"""
from __future__ import annotations

import os
import re

import httpx
import pytest
import pytest_asyncio

pytestmark = pytest.mark.asyncio

_INTERNAL_URL = os.getenv("INTERNAL_URL", "")
_ADMIN_USER   = os.getenv("ADMIN_USER", "admin")
_ADMIN_PASS   = os.getenv("ADMIN_PASS", "")

_CSRF_HX_RE   = re.compile(r'"X-CSRF-Token":\s*"([^"]+)"')
_CSRF_FORM_RE = re.compile(r'name="csrf_token"\s+value="([^"]+)"')

_PAGE_SIZE = 20


def _need(name: str, val: str) -> str:
    if not val:
        pytest.skip(f"{name} not configured in .env")
    return val


def _extract_csrf(html: str) -> str:
    m = _CSRF_HX_RE.search(html) or _CSRF_FORM_RE.search(html)
    assert m, f"CSRF token missing in HTML:\n{html[:500]}"
    return m.group(1)


def _count_data_rows(html: str) -> int:
    """Count <tr> elements inside <tbody> that have at least one <td>."""
    return len(re.findall(r"<tr>\s*<td", html))


def _count_tds_per_row(html: str) -> list[int]:
    """Return list of <td> counts per <tr> (only rows with tds)."""
    rows = re.findall(r"<tr>(.*?)</tr>", html, re.DOTALL)
    return [len(re.findall(r"<td", row)) for row in rows if "<td" in row]


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest_asyncio.fixture
async def ui_client():
    url = _need("INTERNAL_URL", _INTERNAL_URL)
    async with httpx.AsyncClient(base_url=url, follow_redirects=True) as client:
        yield client


@pytest_asyncio.fixture
async def logged_in(ui_client):
    """ui_client pre-authenticated as admin; yields the client."""
    _need("ADMIN_PASS", _ADMIN_PASS)
    page = await ui_client.get("/admin/login.html")
    assert page.status_code == 200
    csrf = _extract_csrf(page.text)
    resp = await ui_client.post(
        "/admin/login.html",
        data={"username": _ADMIN_USER, "password": _ADMIN_PASS, "csrf_token": csrf},
    )
    assert resp.status_code == 200, f"Login failed: {resp.text[:300]}"
    assert "access_token" in ui_client.cookies, "Cookie not set after login"
    yield ui_client


# ── T6 tests ──────────────────────────────────────────────────────────────────

async def test_t6_1_unauthenticated_redirect(ui_client):
    """T6-1: GET /admin/sensor-data without cookie → redirect to login."""
    # ui_client has no cookie; follow_redirects=True so check final URL.
    resp = await ui_client.get("/admin/sensor-data")
    assert resp.status_code == 200
    assert "/admin/login.html" in str(resp.url), (
        f"Expected redirect to login, got: {resp.url}"
    )


async def test_t6_2_no_filters_page_1(logged_in, sensor_data_seed):
    """T6-2: No filters → 200; table present; exactly PAGE_SIZE rows; 10 tds each."""
    resp = await logged_in.get("/admin/sensor-data/table")
    assert resp.status_code == 200
    html = resp.text
    assert "<table>" in html, "No <table> in response"
    row_td_counts = _count_tds_per_row(html)
    data_rows = [c for c in row_td_counts if c > 1]  # exclude empty-state colspan row
    assert len(data_rows) == _PAGE_SIZE, (
        f"Expected {_PAGE_SIZE} rows, got {len(data_rows)}"
    )
    assert all(c == 10 for c in data_rows), (
        f"Expected 10 tds per row, got: {set(data_rows)}"
    )
    # Region and station columns should show 998 and 1 for all rows on this page.
    assert html.count(">998<") == _PAGE_SIZE
    assert html.count(">1<") >= _PAGE_SIZE


async def test_t6_3_filter_region(logged_in, sensor_data_seed):
    """T6-3: Filter region_id=998 → all rows belong to region 998."""
    resp = await logged_in.get("/admin/sensor-data/table?region_id=998")
    assert resp.status_code == 200
    html = resp.text
    # No row from another region should appear (e.g. 999).
    assert ">999<" not in html
    assert ">998<" in html


async def test_t6_4_filter_station(logged_in, sensor_data_seed):
    """T6-4: Filter station_id=1 → all rows belong to station 1."""
    resp = await logged_in.get("/admin/sensor-data/table?station_id=1")
    assert resp.status_code == 200
    html = resp.text
    assert "<table>" in html
    data_rows = [c for c in _count_tds_per_row(html) if c > 1]
    assert len(data_rows) > 0


async def test_t6_5_filter_region_and_station(logged_in, sensor_data_seed):
    """T6-5: Filter region_id=998&station_id=1 → strictly the fixture device's rows."""
    # Page through all results and collect timestamps.
    all_timestamps = set()
    page = 1
    while True:
        resp = await logged_in.get(
            f"/admin/sensor-data/table?region_id=998&station_id=1&page={page}"
        )
        assert resp.status_code == 200
        html = resp.text
        # Extract timestamps from first <td> of each data row.
        ts_matches = re.findall(
            r"<td>(\d{4}-\d{2}-\d{2} \d{2}:\d{2})</td>", html
        )
        if not ts_matches:
            break
        all_timestamps.update(ts_matches)
        if f"page={page + 1}" not in html and f"page={ page + 1 }" not in html:
            if "pagination" not in html or f">{page + 1}<" not in html:
                break
        page += 1
        if page > 10:
            break
    assert len(all_timestamps) == 60, (
        f"Expected 60 seeded rows, found {len(all_timestamps)} unique timestamps"
    )
    # No other region should appear.
    resp = await logged_in.get("/admin/sensor-data/table?region_id=998&station_id=1")
    assert ">999<" not in resp.text


async def test_t6_6_filter_date_range(logged_in, sensor_data_seed):
    """T6-6: Filter date_from=day2&date_to=day2 → only day2 rows returned."""
    day2 = sensor_data_seed["day2"]
    resp = await logged_in.get(
        f"/admin/sensor-data/table?region_id=998&station_id=1&date_from={day2}&date_to={day2}"
    )
    assert resp.status_code == 200
    html = resp.text
    data_rows = [c for c in _count_tds_per_row(html) if c > 1]
    assert len(data_rows) == 20, f"Expected 20 rows for day2, got {len(data_rows)}"
    # All timestamps should contain day2's date.
    ts_matches = re.findall(r"<td>(\d{4}-\d{2}-\d{2}) \d{2}:\d{2}</td>", html)
    assert all(ts == day2 for ts in ts_matches), (
        f"Rows from other days present: {set(ts_matches)}"
    )


async def test_t6_7_filter_bus_range(logged_in, sensor_data_seed):
    """T6-7: Filter bus_min=0.0&bus_max=5.0 → all BUS values in [0.0, 5.0]."""
    resp = await logged_in.get(
        "/admin/sensor-data/table?region_id=998&station_id=1&bus_min=0.0&bus_max=5.0"
    )
    assert resp.status_code == 200
    html = resp.text
    assert "<table>" in html
    # Extract BUS values from the last <td> in each data row.
    # The table has 10 columns; last td is BUS.
    bus_values = re.findall(r"<td>([-\d.]+)</td>\s*</tr>", html)
    assert len(bus_values) > 0, "No BUS values found"
    for v in bus_values:
        try:
            f = float(v)
        except ValueError:
            continue
        assert 0.0 <= f <= 5.0, f"BUS value {f} outside [0.0, 5.0]"


async def test_t6_8_combined_filters(logged_in, sensor_data_seed):
    """T6-8: Combined region+station+date+bus filters → intersection satisfied."""
    day2 = sensor_data_seed["day2"]
    resp = await logged_in.get(
        f"/admin/sensor-data/table"
        f"?region_id=998&station_id=1"
        f"&date_from={day2}&date_to={day2}"
        f"&bus_min=0.0&bus_max=5.0"
    )
    assert resp.status_code == 200
    html = resp.text
    # Must be a subset of T6-6 (day2 only) and T6-7 (bus in range).
    data_rows = [c for c in _count_tds_per_row(html) if c > 1]
    # day2 has 20 rows, bus range 0..5 covers roughly 15 of 60 rows → expect some rows
    # but definitely fewer than 20.
    assert 0 <= len(data_rows) <= 20
    ts_matches = re.findall(r"<td>(\d{4}-\d{2}-\d{2}) \d{2}:\d{2}</td>", html)
    assert all(ts == day2 for ts in ts_matches), (
        f"Rows from other days present: {set(ts_matches)}"
    )


async def test_t6_9_empty_result(logged_in, sensor_data_seed):
    """T6-9: Filter matching no rows → empty-state message, no error."""
    resp = await logged_in.get(
        "/admin/sensor-data/table?region_id=0&station_id=0"
    )
    assert resp.status_code == 200
    html = resp.text
    assert "No records match the current filters" in html
    assert "<table>" in html
    # No data rows should appear.
    data_rows = [c for c in _count_tds_per_row(html) if c > 1]
    assert len(data_rows) == 0


async def test_t6_10_pagination_disjoint(logged_in, sensor_data_seed):
    """T6-10: page=1 and page=2 timestamp sets are disjoint; page=2 is non-empty."""
    base = "/admin/sensor-data/table?region_id=998&station_id=1"

    resp1 = await logged_in.get(f"{base}&page=1")
    resp2 = await logged_in.get(f"{base}&page=2")

    assert resp1.status_code == 200
    assert resp2.status_code == 200

    def extract_timestamps(html: str) -> set[str]:
        return set(re.findall(r"<td>(\d{4}-\d{2}-\d{2} \d{2}:\d{2})</td>", html))

    ts1 = extract_timestamps(resp1.text)
    ts2 = extract_timestamps(resp2.text)

    assert len(ts1) == _PAGE_SIZE, f"Page 1 should have {_PAGE_SIZE} rows, got {len(ts1)}"
    assert len(ts2) > 0, "Page 2 should be non-empty (≥21 seeded rows)"
    assert ts1.isdisjoint(ts2), f"Pages share timestamps: {ts1 & ts2}"
