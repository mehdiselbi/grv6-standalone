// swift-tools-version: 5.9
// GRV6 DJ, Phase 1: audio engine + 4 decks + mixer with a minimal test UI.
// Open this folder in Xcode, pick the iPad, press Run.
import PackageDescription
import AppleProductTypes

let package = Package(
    name: "GRV6DJ",
    platforms: [
        .iOS("17.0")
    ],
    products: [
        .iOSApplication(
            name: "GRV6 DJ",
            targets: ["AppModule"],
            bundleIdentifier: "local.mehdi.GRV6DJ",
            teamIdentifier: "B5JLGJYA59",
            displayVersion: "0.1",
            bundleVersion: "1",
            appIcon: .placeholder(icon: .mic),
            accentColor: .presetColor(.orange),
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
        .target(
            name: "DJCore",
            path: "Sources/DJCore",
            exclude: ["vendor/LICENSE.txt", "vendor/signalsmith-linear/LICENSE.txt"],
            cxxSettings: [
                // DSP must be optimised even in Debug, or 4 keylocked decks will drop out
                .unsafeFlags(["-O3"])
            ],
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("AudioToolbox")
            ]
        ),
        .executableTarget(
            name: "AppModule",
            dependencies: ["DJCore"],
            path: "Sources/App",
            resources: [.copy("Samples")] // owner's own drum sample packs (909, 707), personal use
        )
    ],
    cxxLanguageStandard: .cxx17
)
