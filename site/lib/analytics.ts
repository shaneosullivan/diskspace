import {siteHostname} from "@/lib/site";
import {fitEvent} from "@/lib/track-client";

/// The self-hosted Umami collector. Overridable so a local run can be pointed
/// at something harmless instead of the real one.
const endpoint = process.env.UMAMI_URL ?? "https://chofterstats.com/api/send";
const websiteId = "6709db68-7218-4edd-928e-133f7fbfcca6";

type Metadata = Record<string, string | number | boolean>;

/// Records one server-side event.
///
/// Never throws and never rejects: analytics is not worth failing a request
/// the app is waiting on. Call it from `after()` so it runs once the response
/// has already gone out.
export async function track(
  event: string,
  {
    path,
    ua,
    ip,
    metadata,
  }: {path: string; ua: string; ip?: string; metadata?: Metadata},
): Promise<void> {
  // Local development would otherwise file real events against the live site.
  if (process.env.NODE_ENV !== "production" && !process.env.UMAMI_URL) {
    return;
  }
  try {
    const [name, data] = fitEvent(event, metadata);
    const body = {
      type: "event",
      payload: {
        website: websiteId,
        hostname: siteHostname,
        url: path,
        name,
        ...(data ? {data} : {}),
        // Passed through so the event is attributed to the caller rather than
        // to the server forwarding it.
        userAgent: ua,
        ...(ip ? {ip} : {}),
      },
    };

    await fetch(endpoint, {
      method: "POST",
      // Umami silently drops requests whose agent looks like a bot, which a
      // default library agent does, so the caller's agent is sent here too.
      headers: {"Content-Type": "application/json", "User-Agent": ua},
      body: JSON.stringify(body),
      // A slow collector must not hold the serverless function open.
      signal: AbortSignal.timeout(3000),
    });
  } catch {
    // Deliberately silent: nothing downstream can act on this, and the caller
    // has already answered.
  }
}
