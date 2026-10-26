// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "AynaMac",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "ayna-mac",
            path: "Sources/AynaMac",
            linkerSettings: [
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
            ]
        )
    ]
)
