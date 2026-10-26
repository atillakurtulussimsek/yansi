// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "YansiMac",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "yansi-mac",
            path: "Sources/YansiMac",
            linkerSettings: [
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
            ]
        )
    ]
)
