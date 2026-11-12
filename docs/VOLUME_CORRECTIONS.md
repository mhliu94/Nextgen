# Cumulative volume decreases

The configured policy for cumulative market-volume snapshots is **log and continue**.
A decrease emits an ERROR-level `cumulative_volume_decrease` message with the
symbol, session, feed, sequence, previous count and current count. It does not
create a synthetic trade correction, permanently hold a parent, or invalidate an
otherwise healthy quote/volume feed.

Negative deltas earn no new volume credit. Within a trading session, positive
updates must exceed the previous credited high-water mark to earn additional
credit. This prevents a decrease and rebound from counting the same volume twice.
The watermark persists across reconnects and restarts and is scoped to the feed's
trading-session identifier. A new trading session establishes a fresh baseline;
yesterday's watermark does not suppress today's volume after the pre-open reset.

Legacy `counter_conflict` checkpoint flags do not block the feed under this policy.
Existing persisted parent holds still require separate review; changing the policy
does not erase historical order/fill conflicts or modify executed/reserved shares.
Explicit trade-ID corrections retain their reference validation and accounting.

TradeStation documents corrections to prices, volumes and timestamps:
https://help.tradestation.com/10_00/eng/tradestationhelp/desktop/corrections_deletions_insertions.htm

Regression coverage includes decreases and rebounds, duplicate events, sequence
gaps, reconnect/restart persistence, trading-day rollover, legacy checkpoint flags,
replacement sessions, and genuine referenced/invalid trade corrections.
