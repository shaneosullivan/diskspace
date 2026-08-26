/// Everything that needs the site's own address reads it from here.
export const siteUrl = "https://diskspace.io";

/// Where the apps send people who need a newer version.
export const downloadsUrl = `${siteUrl}/#download`;

/// The endpoint the apps ask for the latest released version of themselves.
export const latestVersionUrl = `${siteUrl}/api/latest-version`;
