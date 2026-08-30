#include "tx_history.h"
#include <algorithm>

// Cap on records held in memory (bounds RAM on a busy merchant).
#define MAX_RECORDS  600

// Smallest plausible "clock is set" epoch — 2023-11-14. Before this the
// device is still on its power-on epoch and calendar math is meaningless.
#define CLOCK_SET_THRESHOLD  1700000000L

// Compute the start-of-week (Monday), start-of-this-month and
// start-of-last-month boundaries in local time, as UTC epoch seconds.
static void computeBoundaries(time_t now, time_t& dayAgo, time_t& weekStart,
                              time_t& monthStart, time_t& lastMonthStart) {
    struct tm lt;
    localtime_r(&now, &lt);

    struct tm t0 = lt;            // start of today (local midnight)
    t0.tm_hour = 0; t0.tm_min = 0; t0.tm_sec = 0;

    struct tm tw = t0;           // Monday of this week
    tw.tm_mday -= (lt.tm_wday + 6) % 7;   // tm_wday: Sun=0 -> Mon=0 offset
    weekStart = mktime(&tw);

    struct tm tm1 = t0;          // first of this month
    tm1.tm_mday = 1;
    monthStart = mktime(&tm1);

    struct tm tml = t0;          // first of last month (mktime normalises)
    tml.tm_mday = 1;
    tml.tm_mon -= 1;
    lastMonthStart = mktime(&tml);

    dayAgo = now - 24 * 3600;
}

// Format an epoch as an ISO-8601 UTC string for the API query params.
static String isoUtc(time_t t) {
    struct tm g;
    gmtime_r(&t, &g);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    return String(buf);
}

HistoryData buildHistory(PaymentProvider& api) {
    HistoryData d = {};

    // Providers without a history endpoint: say so before anything else
    // (the clock check below would otherwise mask it right after boot).
    if (!api.supportsHistory()) {
        d.error = "Transaction history not supported";
        return d;
    }

    d.now = time(nullptr);
    if (d.now < CLOCK_SET_THRESHOLD) {
        d.error = "Clock not set";
        return d;
    }

    computeBoundaries(d.now, d.dayAgo, d.weekStart, d.monthStart, d.lastMonthStart);

    // The endpoint returns a date range in one shot (limit/offset are ignored
    // when from/to are supplied), so we fetch the whole window at once.
    String fromIso = isoUtc(d.lastMonthStart);
    String toIso   = isoUtc(d.now);

    TxPage p = api.getTransactions(fromIso, toIso, 0, 0);
    if (!p.ok) {
        d.error = p.error.length() ? p.error : "Fetch failed";
        return d;
    }

    for (const TxRecord& r : p.records) {
        if (r.createdAt == 0) continue;               // unparseable timestamp
        if ((int)d.all.size() >= MAX_RECORDS) { d.truncated = true; break; }
        d.all.push_back(r);
    }
    // The server returned fewer records than it says exist in the range.
    if (p.total > p.rawCount) d.truncated = true;

    // Newest first.
    std::sort(d.all.begin(), d.all.end(),
              [](const TxRecord& a, const TxRecord& b) {
                  return a.createdAt > b.createdAt;
              });

    d.ok = true;
    return d;
}

void timeframeRange(const HistoryData& d, Timeframe tf, time_t& start, time_t& end) {
    switch (tf) {
        case Timeframe::DAY:        start = d.dayAgo;         end = d.now;          break;
        case Timeframe::WEEK:       start = d.weekStart;      end = d.now;          break;
        case Timeframe::MONTH:      start = d.monthStart;     end = d.now;          break;
        case Timeframe::LAST_MONTH: start = d.lastMonthStart; end = d.monthStart;   break;
        default:                    start = d.dayAgo;         end = d.now;          break;
    }
}

const char* timeframeLabel(Timeframe tf) {
    switch (tf) {
        case Timeframe::DAY:        return "24h";
        case Timeframe::WEEK:       return "Week";
        case Timeframe::MONTH:      return "Month";
        case Timeframe::LAST_MONTH: return "Last mo";
        default:                    return "24h";
    }
}
