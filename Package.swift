// swift-tools-version:6.0

import PackageDescription

let package = Package(
    name: "lzfse",
    products: [
        .library(name: "lzfse", targets: ["lzfse"]),
    ],
    targets: [
        // Decoder only: the encoder and the CLI are left out.
        .target(name: "lzfse",
                path: "src",
                exclude: [
                    "lzfse_main.c",
                    "lzfse_encode.c",
                    "lzfse_encode_base.c",
                    "lzvn_encode_base.c",
                ],
                publicHeadersPath: "include"),
        .testTarget(name: "lzfseTests",
                    dependencies: ["lzfse"],
                    path: "tests/lzfseTests"),
    ]
)
