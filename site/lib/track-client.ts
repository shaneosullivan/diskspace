/// Browser-side events, sent through the Umami script in the layout.
///
/// The script is deferred, so a click landing before it has loaded is not
/// recorded; that is a rare, accepted loss.
type EventData = Record<string, string | number | boolean>;

declare global {
  interface Window {
    umami?: {
      track: (name: string, data?: EventData) => void;
    };
  }
}

// Umami's limits: 50 characters for an event name, 500 for a string value.
// Longer input is trimmed to fit rather than the event being dropped.
export function fitEvent(
  event: string,
  metadata?: EventData,
): [string, EventData | undefined] {
  const data = metadata
    ? Object.fromEntries(
        Object.entries(metadata).map(([k, v]) => [
          k,
          typeof v === "string" ? v.slice(0, 500) : v,
        ]),
      )
    : undefined;
  return [event.slice(0, 50), data];
}

export function trackClient(event: string, metadata?: EventData): void {
  window.umami?.track(...fitEvent(event, metadata));
}
