import type {NextConfig} from "next";

const nextConfig: NextConfig = {
  // Not a static export: the pages are all prerendered at build time, but
  // /api/latest-version runs per request so usage can be recorded server side.
  images: {unoptimized: true},
  // Pin the workspace root: there are lockfiles above this directory, and
  // Turbopack otherwise picks one of those and warns.
  turbopack: {root: import.meta.dirname},
};

export default nextConfig;
