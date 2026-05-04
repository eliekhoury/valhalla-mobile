// swift-tools-version:5.8
import Foundation
import PackageDescription

// Use the local binary if true. Default: respect VALHALLA_MOBILE_DEV
// env var; otherwise pull the published release. Local override file
// `LOCAL_BINARY` (any content) lets a consuming Xcode project pin the
// local xcframework without setting an env var on every build — the
// flag travels with the source tree.
let useLocalBinary: Bool = {
    if let envFlag = Context.environment["VALHALLA_MOBILE_DEV"].flatMap(Bool.init) {
        return envFlag
    }
    return FileManager.default.fileExists(
        atPath: Context.packageDirectory + "/LOCAL_BINARY"
    )
}()

// Use the local binary
var binaryTarget: Target = .binaryTarget(
    name: "ValhallaWrapper",
    path: "build/apple/valhalla-wrapper.xcframework"
)

// CI will replace the nils with the actual values when building a release
// Bike IQ fork: pinned to our extended-actions release on the eliekhoury
// fork until the upstream PR merges and a new Rallista release is cut.
let version: String = "0.5.1-bikeiq.1"
let binaryURL: String =
    "https://github.com/eliekhoury/valhalla-mobile/releases/download/\(version)/valhalla-wrapper.xcframework.zip"
let binaryChecksum: String = "89f99e4ff3e816bb8f41639667d275da4044194ea3a46bc24062a07b74b260f0"

if !useLocalBinary {
    binaryTarget = .binaryTarget(
        name: "ValhallaWrapper",
        url: binaryURL,
        checksum: binaryChecksum
    )
}

let package = Package(
    name: "ValhallaMobile",
    platforms: [
        .iOS("16.4")
        // .tvOS(.v13),
        // .watchOS(.v6),
        // .macOS(.v10_13)
    ],
    products: [
        .library(
            name: "Valhalla",
            targets: ["Valhalla"]
        )
    ],
    dependencies: [
        .package(
            url: "https://github.com/Rallista/valhalla-openapi-models-swift.git", .upToNextMinor(from: "0.2.0")),
        .package(url: "https://github.com/UInt2048/Light-Swift-Untar.git", .upToNextMajor(from: "1.0.4")),
        .package(url: "https://github.com/apple/swift-docc-plugin", .upToNextMajor(from: "1.0.0")),
    ],
    targets: [
        .target(
            name: "Valhalla",
            dependencies: [
                "ValhallaObjc",
                "ValhallaWrapper",
                .product(name: "ValhallaConfigModels", package: "valhalla-openapi-models-swift"),
                .product(name: "ValhallaModels", package: "valhalla-openapi-models-swift"),
                .product(name: "Light-Swift-Untar", package: "Light-Swift-Untar"),
            ],
            path: "apple/Sources/Valhalla",
            resources: [
                .process("SupportData")
            ]
        ),
        .target(
            name: "ValhallaObjc",
            dependencies: ["ValhallaWrapper"],
            path: "apple/Sources/ValhallaObjc",
            linkerSettings: [.linkedLibrary("z")]
        ),
        binaryTarget,
        .testTarget(
            name: "ValhallaTests",
            dependencies: ["Valhalla"],
            path: "apple/Tests/ValhallaTests",
            resources: [.copy("TestData")]
        ),
    ],
    cLanguageStandard: .gnu17,
    cxxLanguageStandard: .cxx20
)
