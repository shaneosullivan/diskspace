/// Browser-side events, sent through the Simple Analytics script in the layout.
///
/// The layout also installs the queue stub their docs describe, so a click that
/// lands before the script has finished loading is held and sent afterwards
/// rather than lost.
declare global {
  interface Window {
    sa_event?: (
      event: string,
      metadata?: Record<string, string | number | boolean>,
    ) => void;
  }
}

export function trackClient(
  event: string,
  metadata?: Record<string, string | number | boolean>,
): void {
  window.sa_event?.(event, metadata);
}
