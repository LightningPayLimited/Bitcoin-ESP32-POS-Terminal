#pragma once
#include <Arduino.h>
#include <time.h>
#include <vector>
#include "payment_provider.h"

// ============================================================
// Transaction history.
//
// Pulls the provider's transaction list for [last-month-start .. now] in one
// paged sweep and keeps the individual records. The history screen then
// filters them by a selected timeframe (24h / this week / this month / last
// month) and lists them, with a count + paid total for the period.
//
// Calendar periods are computed in the device's local timezone (set via
// configTzTime at boot), so a real wall-clock is required: buildHistory()
// returns ok == false if the clock hasn't been synced yet.
// ============================================================

enum class Timeframe { DAY, WEEK, MONTH, LAST_MONTH };

struct HistoryData {
    bool   ok;
    String error;

    // All records in [lastMonthStart, now], newest first.
    std::vector<TxRecord> all;
    bool   truncated;     // hit the fetch cap — older records omitted

    // Period boundaries (UTC epoch, derived from local calendar time).
    time_t now;
    time_t dayAgo;
    time_t weekStart;
    time_t monthStart;
    time_t lastMonthStart;
};

// Fetch + page the history. Blocks while paging the API.
HistoryData buildHistory(PaymentProvider& api);

// Half-open [start, end) epoch range for a timeframe within `d`.
void timeframeRange(const HistoryData& d, Timeframe tf, time_t& start, time_t& end);

// Short tab label for a timeframe ("24h", "Week", ...).
const char* timeframeLabel(Timeframe tf);
