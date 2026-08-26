import {siteHostname} from "@/lib/site";

/// Simple Analytics' server-side queue. Overridable so a local run can be
/// pointed at something harmless instead of the real one.
const endpoint =
  process.env.SIMPLE_ANALYTICS_URL ??
  "https://queue.simpleanalyticscdn.com/events";

type Metadata = Record<string, string | number | boolean>;

/// Records one server-side event.
///
/// Never throws and never rejects: analytics is not worth failing a request
/// the app is waiting on. Call it from `after()` so it runs once the response
/// has already gone out.
export async function track(
  event: string,
  {path, ua, metadata}: {path: string; ua: string; metadata?: Metadata},
): Promise<void> {
  // Local development would otherwise file real events against the live site.
  if (
    process.env.NODE_ENV !== "production" &&
    !process.env.SIMPLE_ANALYTICS_URL
  ) {
    return;
  }
  try {
    await fetch(endpoint, {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({
        type: "event",
        hostname: siteHostname,
        event,
        path,
        https: true,
        // Their docs warn that a default library user agent gets read as a
        // robot and dropped, so the caller says plainly what it is.
        ua,
        ...(metadata ? {metadata} : {}),
      }),
      // A slow queue must not hold the serverless function open.
      signal: AbortSignal.timeout(3000),
    });
  } catch {
    // Deliberately silent: nothing downstream can act on this, and the caller
    // has already answered.
  }
}
