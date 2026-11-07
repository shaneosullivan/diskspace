/// Everything that needs the site's own address reads it from here.
///
/// With the www, because that is where the host actually serves the site from:
/// the bare domain answers every request with a 308 to this one. A canonical
/// link pointing at a URL that redirects away is a signal that argues with
/// itself, and it was pointing at the bare domain.
export const siteUrl = "https://www.diskspace.io";

/// Where the apps send people who need a newer version.
export const downloadsUrl = `${siteUrl}/#download`;

/// Linux has three formats to choose between, so it has a page of its own.
export const linuxDownloadPath = "/download/linux";
export const linuxDownloadsUrl = `${siteUrl}${linuxDownloadPath}`;

/// The endpoint the apps ask for the latest released version of themselves.
export const latestVersionUrl = `${siteUrl}/api/latest-version`;

/// The bare domain, which is how analytics identifies the site.
export const siteHostname = new URL(siteUrl).hostname;

/// Where the source code lives, linked from the site header.
export const repoUrl = "https://github.com/shaneosullivan/diskspace";
