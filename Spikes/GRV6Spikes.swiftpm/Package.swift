// swift-tools-version: 5.8
// Swift Playgrounds style app package. Open this folder in Xcode, pick the iPad, press Run.
import PackageDescription
import AppleProductTypes

let package = Package(
    name: "GRV6Spikes",
    platforms: [
        .iOS("16.0")
    ],
    products: [
        .iOSApplication(
            name: "GRV6Spikes",
            targets: ["AppModule"],
            bundleIdentifier: "local.mehdi.GRV6Spikes",
            teamIdentifier: "",
            displayVersion: "0.1",
            bundleVersion: "1",
            appIcon: .placeholder(icon: .mic),
            accentColor: .presetColor(.blue),
            supportedDeviceFamilies: [
                .pad
            ],
            supportedInterfaceOrientations: [
                .landscapeRight,
                .landscapeLeft
            ]
        )
    ],
    targets: [
        .executableTarget(
            name: "AppModule",
            path: "."
        )
    ]
)
