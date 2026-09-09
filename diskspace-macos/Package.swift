// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "diskscan",
    platforms: [.macOS(.v15)],
    targets: [
        .target(
            name: "DiskScanCore",
            swiftSettings: [
                .swiftLanguageMode(.v5),
                .unsafeFlags(["-Ounchecked"], .when(configuration: .release)),
            ]
        ),
        .executableTarget(
            name: "DiskSpaceApp",
            dependencies: ["DiskScanCore"],
            resources: [.process("Resources")],
            swiftSettings: [
                .swiftLanguageMode(.v5),
                .unsafeFlags(["-Ounchecked"], .when(configuration: .release)),
            ]
        ),
        .executableTarget(
            name: "diskscan",
            dependencies: ["DiskScanCore"],
            swiftSettings: [
                .swiftLanguageMode(.v5),
                .unsafeFlags(["-Ounchecked"], .when(configuration: .release)),
            ]
        ),
        .testTarget(
            name: "DiskScanCoreTests",
            dependencies: ["DiskScanCore"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
